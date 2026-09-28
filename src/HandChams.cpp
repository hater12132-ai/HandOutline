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

// Bedrock Color — static storage (engine may keep pointer)
struct Color {
    float r, g, b, a;
};

constexpr const char* kModuleId = "bactro.handchams";

std::atomic_bool g_enabled{false};
std::atomic_bool g_handOnly{false}; // default OFF = hand + entities/players
std::atomic_bool g_whiteOutline{true};
// Snowy palette (ice / fresh snow)
std::atomic<float> g_r{0.88f};
std::atomic<float> g_g{0.95f};
std::atomic<float> g_b{1.00f};
std::atomic<float> g_opacity{0.55f};
std::atomic<float> g_intensity{1.35f};
std::atomic_int g_hits{0};

Color g_chams{0.88f, 0.95f, 1.00f, 0.55f};
Color g_outline{1.00f, 1.00f, 1.00f, 0.95f}; // glowy white outline / glint

bool g_renderFpHooked = false; // used by shouldApply before tryInstallHooks

void logLine(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    HC_LOGI("%s", buf);
}

void refresh() {
    const float i = g_intensity.load(std::memory_order_relaxed);
    float aa = g_opacity.load(std::memory_order_relaxed);
    if (aa < 0.05f) aa = 0.05f;
    if (aa > 1.0f) aa = 1.0f;
    g_chams = {
        g_r.load(std::memory_order_relaxed) * i,
        g_g.load(std::memory_order_relaxed) * i,
        g_b.load(std::memory_order_relaxed) * i,
        aa,
    };
    // pure white glow edge
    g_outline = {1.0f, 1.0f, 1.0f, 0.95f};
}

bool shouldApply() {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    // Hand/items only: prefer FP-hand phase when that hook works.
    // If renderFirstPerson never hooked, still apply so hand/items get snowy tint.
    if (g_handOnly.load(std::memory_order_relaxed)) {
        if (g_renderFpHooked)
            return bactro::phase::inFirstPersonHand.load(std::memory_order_acquire);
        return true; // fallback — otherwise chams never fire
    }
    return true; // all entities
}

// ---- FP hand: set flag so item/hand draws get chams (NO extra 2D draw) ----
using RenderFirstPersonFn = void (*)(void*, void*, void*, void*, void*, void*);
RenderFirstPersonFn g_renderFpOriginal = nullptr;

void renderFirstPersonDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!g_renderFpOriginal) return;
    bactro::phase::inFirstPersonHand.store(true, std::memory_order_release);
    g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    bactro::phase::inFirstPersonHand.store(false, std::memory_order_release);
}

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

    if (!shouldApply()) {
        g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                             overlay, changeColor, changeColor2, glintColor, glintUVScale, uvAnim, uvOffset1,
                             uvOffset2, uvRot1, uvRot2);
        return;
    }

    refresh();
    const Color* glint = glintColor;
    if (g_whiteOutline.load(std::memory_order_relaxed))
        glint = &g_outline;
    // Wash entity/hand mesh: override light + overlay + change colors (stronger than overlay-only)
    g_setEntityConstants(entityConstants, renderContext, &g_chams, tileLightColorUV, blockLightColor,
                         &g_chams, &g_chams, &g_chams, glint, glintUVScale, uvAnim, uvOffset1, uvOffset2,
                         uvRot1, uvRot2);

    const int n = g_hits.fetch_add(1, std::memory_order_relaxed);
    if (n < 12)
        logLine("SnowChams: entityConstants #%d rgba=%.2f,%.2f,%.2f,%.2f outline=%d", n, g_chams.r, g_chams.g,
                g_chams.b, g_chams.a, g_whiteOutline.load() ? 1 : 0);
}

using SetupActorGlintFn = void (*)(
    void*, void*, void*, const Color*, const Color*, const Color*, const Color*, float, float, float, float,
    const void*);

SetupActorGlintFn g_setupActorGlint = nullptr;
bool g_hookedActor = false;

// Same layout as ActorGlint — used for non-actor / item glint setup (broader coverage)
using SetupGlintFn = void (*)(
    void*, void*, void*, const Color*, const Color*, const Color*, const Color*, float, float, float, float,
    const void*);
SetupGlintFn g_setupGlint = nullptr;
bool g_hookedGlint = false;

void setupGlintDetour(
    void* screenContext, void* entityContext, void* actor, const Color* overlay, const Color* changeColor,
    const Color* changeColor2, const Color* glintColor, float uvOffset1, float uvOffset2, float uvRot1,
    float uvRot2, const void* lightEmissionColor) {
    if (!g_setupGlint) return;
    if (!shouldApply()) {
        g_setupGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor,
                     uvOffset1, uvOffset2, uvRot1, uvRot2, lightEmissionColor);
        return;
    }
    refresh();
    const Color* glint = glintColor;
    if (g_whiteOutline.load(std::memory_order_relaxed))
        glint = &g_outline;
    g_setupGlint(screenContext, entityContext, actor, &g_chams, &g_chams, &g_chams, glint, uvOffset1,
                 uvOffset2, uvRot1, uvRot2, lightEmissionColor);
}

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

    refresh();
    const Color* glint = glintColor;
    if (g_whiteOutline.load(std::memory_order_relaxed))
        glint = &g_outline;
    g_setupActorGlint(screenContext, entityContext, actor, &g_chams, &g_chams, &g_chams, glint, uvOffset1,
                      uvOffset2, uvRot1, uvRot2, lightEmissionColor);
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
        } else
            logLine("HandChams: renderFirstPerson FAIL");
    }

    if (!g_hookedEntity) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetEntityConstants,
                                 reinterpret_cast<void*>(&setEntityConstantsDetour), &o) &&
            o) {
            g_setEntityConstants = reinterpret_cast<SetEntityConstantsFn>(o);
            g_hookedEntity = true;
            logLine("HandChams: setEntityConstants hooked");
        } else
            logLine("HandChams: setEntityConstants FAIL");
    }

    if (!g_hookedActor) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetupShaderParametersActorGlint,
                                 reinterpret_cast<void*>(&setupActorGlintDetour), &o) &&
            o) {
            g_setupActorGlint = reinterpret_cast<SetupActorGlintFn>(o);
            g_hookedActor = true;
            logLine("HandChams: setupActorGlint hooked");
        } else
            logLine("HandChams: setupActorGlint FAIL");
    }


    if (!g_hookedGlint) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetupShaderParametersGlint,
                                 reinterpret_cast<void*>(&setupGlintDetour), &o) &&
            o) {
            g_setupGlint = reinterpret_cast<SetupGlintFn>(o);
            g_hookedGlint = true;
            logLine("HandChams: setupGlint hooked");
        } else
            logLine("HandChams: setupGlint FAIL");
    }

    logLine("HandChams: hooks fp=%d entity=%d actor=%d glint=%d", g_renderFpHooked ? 1 : 0,
            g_hookedEntity ? 1 : 0, g_hookedActor ? 1 : 0, g_hookedGlint ? 1 : 0);
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
        else if (key == "opacity")
            g_opacity.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "intensity")
            g_intensity.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "handOnly")
            g_handOnly.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "whiteOutline")
            g_whiteOutline.store(value == "true" || value == "1", std::memory_order_relaxed);
        refresh();
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Snow Chams");
    b.description(
         "Snowy mesh chams on hand + entities/players (Hand-only off). White glint outline when supported. "
         "F5/third-person to see your own body. Join world, then enable.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("r", "Red", pl::modmenu::ConfigType::SliderFloat, "0.88", "0", "1", "");
    b.config("g", "Green", pl::modmenu::ConfigType::SliderFloat, "0.95", "0", "1", "");
    b.config("b", "Blue", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("opacity", "Opacity", pl::modmenu::ConfigType::SliderFloat, "0.55", "0.05", "1.0", "");
    b.config("intensity", "Intensity", pl::modmenu::ConfigType::SliderFloat, "1.35", "0.1", "2.5", "");
    b.config("whiteOutline", "White glow outline", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("handOnly", "Hand/items only", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.registerModule();
}

void onSignaturesReady() {
    logLine("HandChams: ready — join world, then enable");
}

void shutdown() { g_enabled.store(false, std::memory_order_release); }

void onPostFrame() {
    // intentionally empty — no 2D overlay
}

} // namespace bactro::handchams
