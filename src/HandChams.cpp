#include "bactro/HandChams.hpp"
#include "bactro/RenderPhase.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <android/log.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#define HC_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::handchams {
namespace {

struct Color {
    float r, g, b, a;
};

constexpr const char* kModuleId = "bactro.handchams";

std::atomic_bool g_enabled{false}; // off until after join (safe)
std::atomic_bool g_handOnly{false}; // false = hand + visible players (no wallhack)
std::atomic<float> g_r{0.20f};
std::atomic<float> g_g{0.90f};
std::atomic<float> g_b{1.00f};
std::atomic<float> g_intensity{1.0f}; // fill strength
std::atomic_int g_hits{0};
std::atomic_int g_enters{0};

// Persistent — engine may keep pointers past the call
Color g_chamsColor{0.20f, 0.90f, 1.00f, 1.0f};

void logLine(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    HC_LOGI("%s", buf);
}

void refreshColor() {
    const float i = g_intensity.load(std::memory_order_relaxed);
    g_chamsColor = {
        g_r.load(std::memory_order_relaxed) * i,
        g_g.load(std::memory_order_relaxed) * i,
        g_b.load(std::memory_order_relaxed) * i,
        1.0f,
    };
}

bool shouldApply() {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    if (g_handOnly.load(std::memory_order_relaxed))
        return bactro::phase::inFirstPersonHand.load(std::memory_order_acquire);
    // Visible models only: we never disable depth → no wallhack
    return true;
}

// ---- FP hand marker ----
using RenderFirstPersonFn = void (*)(void*, void*, void*, void*, void*, void*);
RenderFirstPersonFn g_renderFpOriginal = nullptr;
bool g_renderFpHooked = false;

void renderFirstPersonDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!g_renderFpOriginal) return;
    bactro::phase::inFirstPersonHand.store(true, std::memory_order_release);
    g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    bactro::phase::inFirstPersonHand.store(false, std::memory_order_release);
}

// ---- setEntityConstants: solid chams fill ----
using SetEntityConstantsFn = void (*)(
    void*, void*, const Color*, const void*, const void*, const Color*, const Color*, const Color*,
    const Color*, const void*, const void*, float, float, float, float);

SetEntityConstantsFn g_setEntityConstants = nullptr;
bool g_hookedEntity = false;

void setEntityConstantsDetour(
    void* entityConstants, void* renderContext, const Color* tileLightColor, const void* tileLightColorUV,
    const void* blockLightColor, const Color* overlay, const Color* changeColor, const Color* changeColor2,
    const Color* glintColor, const void* glintUVScale, const void* uvAnim, float uvOffset1, float uvOffset2,
    float uvRot1, float uvRot2) {
    if (!g_setEntityConstants) return;

    const int enter = g_enters.fetch_add(1, std::memory_order_relaxed);
    if (enter < 8)
        logLine("HandChams: ENTER #%d en=%d apply=%d", enter, g_enabled.load() ? 1 : 0,
                shouldApply() ? 1 : 0);

    if (!shouldApply()) {
        g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                             overlay, changeColor, changeColor2, glintColor, glintUVScale, uvAnim, uvOffset1,
                             uvOffset2, uvRot1, uvRot2);
        return;
    }

    refreshColor();
    // Solid chams: changeColor + overlay both set to configured color (BedrockTools-style)
    g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                         &g_chamsColor, &g_chamsColor, &g_chamsColor, glintColor, glintUVScale, uvAnim,
                         uvOffset1, uvOffset2, uvRot1, uvRot2);

    const int n = g_hits.fetch_add(1, std::memory_order_relaxed);
    if (n < 10)
        logLine("HandChams: chams #%d rgb=%.2f,%.2f,%.2f fp=%d", n, g_chamsColor.r, g_chamsColor.g,
                g_chamsColor.b, bactro::phase::inFirstPersonHand.load() ? 1 : 0);
}

// ---- enchanted / glint path ----
using SetupActorGlintFn = void (*)(
    void*, void*, void*, const Color*, const Color*, const Color*, const Color*, float, float, float, float,
    const void*);

SetupActorGlintFn g_setupActorGlint = nullptr;
bool g_hookedActor = false;

void setupActorGlintDetour(
    void* screenContext, void* entityContext, void* actor, const Color* overlay, const Color* changeColor,
    const Color* changeColor2, const Color* glintColor, float uvOffset1, float uvOffset2, float uvRot1,
    float uvRot2, const void* lightEmissionColor) {
    if (!g_setupActorGlint) return;

    if (!shouldApply()) {
        g_setupActorGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor,
                          uvOffset1, uvOffset2, uvRot1, uvRot2, lightEmissionColor);
        return;
    }

    refreshColor();
    g_setupActorGlint(screenContext, entityContext, actor, &g_chamsColor, &g_chamsColor, &g_chamsColor,
                      glintColor, uvOffset1, uvOffset2, uvRot1, uvRot2, lightEmissionColor);
}

void tryInstallHooks() {
    void* o = nullptr;

    if (!g_renderFpHooked) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ItemInHandRendererRenderFirstPerson,
                                 reinterpret_cast<void*>(&renderFirstPersonDetour), &o) &&
            o) {
            g_renderFpOriginal = reinterpret_cast<RenderFirstPersonFn>(o);
            g_renderFpHooked = true;
            logLine("HandChams: renderFirstPerson hooked");
        } else {
            logLine("HandChams: renderFirstPerson FAIL");
        }
    }

    if (!g_hookedEntity) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetEntityConstants,
                                 reinterpret_cast<void*>(&setEntityConstantsDetour), &o) &&
            o) {
            g_setEntityConstants = reinterpret_cast<SetEntityConstantsFn>(o);
            g_hookedEntity = true;
            logLine("HandChams: setEntityConstants hooked");
        } else {
            logLine("HandChams: setEntityConstants FAIL");
        }
    }

    if (!g_hookedActor) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetupShaderParametersActorGlint,
                                 reinterpret_cast<void*>(&setupActorGlintDetour), &o) &&
            o) {
            g_setupActorGlint = reinterpret_cast<SetupActorGlintFn>(o);
            g_hookedActor = true;
            logLine("HandChams: setupActorGlint hooked");
        } else {
            logLine("HandChams: setupActorGlint FAIL");
        }
    }

    logLine("HandChams: hooks fp=%d entity=%d actor=%d", g_renderFpHooked ? 1 : 0, g_hookedEntity ? 1 : 0,
            g_hookedActor ? 1 : 0);
}

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    logLine("HandChams %s", enabled ? "ON" : "OFF");
    if (enabled) tryInstallHooks();
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "r")
            g_r.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "g")
            g_g.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "b")
            g_b.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "intensity")
            g_intensity.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "handOnly")
            g_handOnly.store(value == "true" || value == "1", std::memory_order_relaxed);
        refreshColor();
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Hand Chams");
    b.description(
         "Solid chams on hand, held items, and visible players. "
         "No wallhack (depth stays on). RGB like BedrockTools. "
         "Join world first, then enable.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("r", "Red", pl::modmenu::ConfigType::SliderFloat, "0.20", "0", "1", "");
    b.config("g", "Green", pl::modmenu::ConfigType::SliderFloat, "0.90", "0", "1", "");
    b.config("b", "Blue", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("intensity", "Intensity", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.1", "2.0", "");
    b.config("handOnly", "Hand only (off = players too)", pl::modmenu::ConfigType::Toggle, "false", "", "",
             "");
    b.registerModule();
}

void onSignaturesReady() {
    logLine("HandChams: ready — enable module after you join the world");
}

void shutdown() { g_enabled.store(false, std::memory_order_release); }

} // namespace bactro::handchams
