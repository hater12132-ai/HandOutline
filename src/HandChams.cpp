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

// Bedrock Color { r, g, b, a } — must outlive the call if the engine stores the pointer
struct Color {
    float r, g, b, a;
};

constexpr const char* kModuleId = "bactro.handchams";

std::atomic_bool g_enabled{false};
std::atomic<float> g_r{0.15f};
std::atomic<float> g_g{0.85f};
std::atomic<float> g_b{1.00f};
std::atomic<float> g_fill{1.0f};
std::atomic<float> g_glow{0.35f};
std::atomic_int g_hits{0};
std::atomic_int g_enters{0};

// Persistent storage so pointers passed into the game stay valid
Color g_fillColor{0.15f, 0.85f, 1.0f, 1.0f};
Color g_overlayColor{1.0f, 1.0f, 1.05f, 0.2f};

void logLine(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    HC_LOGI("%s", buf);
}

void refreshColors() {
    const float f = g_fill.load(std::memory_order_relaxed);
    g_fillColor = {
        g_r.load(std::memory_order_relaxed) * f,
        g_g.load(std::memory_order_relaxed) * f,
        g_b.load(std::memory_order_relaxed) * f,
        1.0f,
    };
    const float glow = g_glow.load(std::memory_order_relaxed);
    g_overlayColor = {1.0f, 1.0f, 1.05f, 0.05f + glow * 0.45f};
}

// ---- renderFirstPerson: mark FP hand only (no color logic here) ----
using RenderFirstPersonFn = void (*)(void*, void*, void*, void*, void*, void*);
RenderFirstPersonFn g_renderFpOriginal = nullptr;
bool g_renderFpHooked = false;

void renderFirstPersonDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!g_renderFpOriginal) return;
    // Always forward first — never skip the real render (crash-safe)
    bactro::phase::inFirstPersonHand.store(true, std::memory_order_release);
    g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    bactro::phase::inFirstPersonHand.store(false, std::memory_order_release);
}

// ---- setEntityConstants ----
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
    if (enter < 12)
        logLine("HandChams: setEntityConstants ENTER #%d en=%d", enter, g_enabled.load() ? 1 : 0);

    if (!g_enabled.load(std::memory_order_relaxed)) {
        g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                             overlay, changeColor, changeColor2, glintColor, glintUVScale, uvAnim, uvOffset1,
                             uvOffset2, uvRot1, uvRot2);
        return;
    }

    // Static storage — do NOT pass stack addresses
    refreshColors();
    g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                         &g_overlayColor, &g_fillColor, &g_fillColor, glintColor, glintUVScale, uvAnim,
                         uvOffset1, uvOffset2, uvRot1, uvRot2);

    const int n = g_hits.fetch_add(1, std::memory_order_relaxed);
    if (n < 12)
        logLine("HandChams: inject #%d rgb=%.2f,%.2f,%.2f", n, g_fillColor.r, g_fillColor.g, g_fillColor.b);
}

// ---- ActorGlint (enchanted) — same static colors ----
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

    if (!g_enabled.load(std::memory_order_relaxed)) {
        g_setupActorGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor,
                          uvOffset1, uvOffset2, uvRot1, uvRot2, lightEmissionColor);
        return;
    }

    refreshColors();
    g_setupActorGlint(screenContext, entityContext, actor, &g_overlayColor, &g_fillColor, &g_fillColor,
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
            logLine("HandChams: renderFirstPerson FAIL (skip)");
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
            logLine("HandChams: setEntityConstants FAIL (skip)");
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
            logLine("HandChams: setupActorGlint FAIL (skip)");
        }
    }

    logLine("HandChams: hooks fp=%d entity=%d actor=%d", g_renderFpHooked ? 1 : 0, g_hookedEntity ? 1 : 0,
            g_hookedActor ? 1 : 0);
}

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    logLine("HandChams %s", enabled ? "ON" : "OFF");
    if (enabled) {
        // Install only when user turns it on — avoids crash on world join
        tryInstallHooks();
    }
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "r")
            g_r.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "g")
            g_g.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "b")
            g_b.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "fill")
            g_fill.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "glow")
            g_glow.store(std::stof(std::string(value)), std::memory_order_relaxed);
        refreshColors();
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Hand Chams");
    b.description(
         "Visible-model color tint (hand + players). No wallhack. RGB + glow. "
         "If the game crashes, turn this module OFF.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("r", "Red", pl::modmenu::ConfigType::SliderFloat, "0.15", "0", "1", "");
    b.config("g", "Green", pl::modmenu::ConfigType::SliderFloat, "0.85", "0", "1", "");
    b.config("b", "Blue", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("fill", "Fill strength", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.1", "1.5", "");
    b.config("glow", "Outer glow", pl::modmenu::ConfigType::SliderFloat, "0.35", "0", "1", "");
    b.registerModule();
}

void onSignaturesReady() {
    logLine("HandChams: ready (enable module to install color hooks)");
}

void shutdown() { g_enabled.store(false, std::memory_order_release); }

} // namespace bactro::handchams
