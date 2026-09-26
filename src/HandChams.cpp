#include "bactro/HandChams.hpp"
#include "bactro/RenderPhase.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cmath>
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

std::atomic_bool g_enabled{false};
std::atomic_bool g_handOnly{false};
std::atomic_bool g_outline{true};   // dual-pass white rim attempt on FP hand
std::atomic_bool g_galaxy{false};   // animated galaxy fill
std::atomic<float> g_r{0.20f};
std::atomic<float> g_g{0.90f};
std::atomic<float> g_b{1.00f};
std::atomic<float> g_intensity{1.0f};
std::atomic<float> g_opacity{1.0f}; // 0..1 alpha of chams
std::atomic_int g_hits{0};

Color g_chamsColor{0.20f, 0.90f, 1.00f, 1.0f};
Color g_outlineColor{1.0f, 1.0f, 1.05f, 1.0f};

std::atomic_bool g_outlinePass{false};

void logLine(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    HC_LOGI("%s", buf);
}

float nowSec() {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<float>(clock::now() - t0).count();
}

void refreshColor() {
    const float i = g_intensity.load(std::memory_order_relaxed);
    const float a = g_opacity.load(std::memory_order_relaxed);

    if (g_galaxy.load(std::memory_order_relaxed)) {
        const float t = nowSec();
        // Moving galaxy-ish palette (navy → purple → cyan + sparkle)
        const float n1 = 0.5f + 0.5f * std::sin(t * 0.7f);
        const float n2 = 0.5f + 0.5f * std::sin(t * 1.3f + 1.7f);
        const float spark = 0.5f + 0.5f * std::sin(t * 5.5f);
        float r = 0.08f + 0.25f * n2 + 0.15f * spark;
        float g = 0.10f + 0.35f * n1 + 0.10f * spark;
        float b = 0.35f + 0.45f * n2 + 0.20f * spark;
        g_chamsColor = {r * i, g * i, b * i, a};
    } else {
        g_chamsColor = {
            g_r.load(std::memory_order_relaxed) * i,
            g_g.load(std::memory_order_relaxed) * i,
            g_b.load(std::memory_order_relaxed) * i,
            a,
        };
    }
    g_outlineColor = {1.0f, 1.0f, 1.05f, a};
}

bool shouldApply() {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    if (g_handOnly.load(std::memory_order_relaxed))
        return bactro::phase::inFirstPersonHand.load(std::memory_order_acquire);
    return true; // visible models only — depth stays on (no wallhack)
}

const Color* injectColor() {
    if (g_outlinePass.load(std::memory_order_acquire))
        return &g_outlineColor;
    return &g_chamsColor;
}

// ---- FP hand: optional dual pass (white then fill) ----
using RenderFirstPersonFn = void (*)(void*, void*, void*, void*, void*, void*);
RenderFirstPersonFn g_renderFpOriginal = nullptr;
bool g_renderFpHooked = false;

void renderFirstPersonDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!g_renderFpOriginal) return;

    bactro::phase::inFirstPersonHand.store(true, std::memory_order_release);

    if (g_enabled.load(std::memory_order_relaxed) && g_outline.load(std::memory_order_relaxed)) {
        // PASS 1 — bright white (outline layer)
        g_outlinePass.store(true, std::memory_order_release);
        g_renderFpOriginal(self, a1, a2, a3, a4, a5);
        g_outlinePass.store(false, std::memory_order_release);
        // PASS 2 — solid chams / galaxy fill
        g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    } else {
        g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    }

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

    refreshColor();
    const Color* c = injectColor();
    g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor, c,
                         c, c, glintColor, glintUVScale, uvAnim, uvOffset1, uvOffset2, uvRot1, uvRot2);

    const int n = g_hits.fetch_add(1, std::memory_order_relaxed);
    if (n < 8)
        logLine("HandChams: chams #%d rgb=%.2f,%.2f,%.2f a=%.2f outlinePass=%d fp=%d", n, c->r, c->g, c->b,
                c->a, g_outlinePass.load() ? 1 : 0,
                bactro::phase::inFirstPersonHand.load() ? 1 : 0);
}

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
    const Color* c = injectColor();
    g_setupActorGlint(screenContext, entityContext, actor, c, c, c, glintColor, uvOffset1, uvOffset2, uvRot1,
                      uvRot2, lightEmissionColor);
}

// Foil + generic glint — more player/item coverage
using SetupFoilFn = void (*)(void*, void*, void*, const Color*, const Color*, float, float, float, float);
SetupFoilFn g_setupFoil = nullptr;
bool g_hookedFoil = false;

void setupFoilDetour(void* a0, void* a1, void* a2, const Color* c0, const Color* c1, float f0, float f1,
                     float f2, float f3) {
    if (!g_setupFoil) return;
    if (!shouldApply()) {
        g_setupFoil(a0, a1, a2, c0, c1, f0, f1, f2, f3);
        return;
    }
    refreshColor();
    const Color* c = injectColor();
    g_setupFoil(a0, a1, a2, c, c, f0, f1, f2, f3);
}

using SetupGlintFn = void (*)(void*, void*, void*, const Color*, const Color*, const Color*, float, float,
                              float, float);
SetupGlintFn g_setupGlint = nullptr;
bool g_hookedGlint = false;

void setupGlintDetour(void* a0, void* a1, void* a2, const Color* c0, const Color* c1, const Color* c2,
                      float f0, float f1, float f2, float f3) {
    if (!g_setupGlint) return;
    if (!shouldApply()) {
        g_setupGlint(a0, a1, a2, c0, c1, c2, f0, f1, f2, f3);
        return;
    }
    refreshColor();
    const Color* c = injectColor();
    g_setupGlint(a0, a1, a2, c, c, c, f0, f1, f2, f3);
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

    if (!g_hookedFoil) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetupFoilShaderParameters,
                                 reinterpret_cast<void*>(&setupFoilDetour), &o) &&
            o) {
            g_setupFoil = reinterpret_cast<SetupFoilFn>(o);
            g_hookedFoil = true;
            logLine("HandChams: setupFoil hooked");
        } else
            logLine("HandChams: setupFoil FAIL");
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

    logLine("HandChams: hooks fp=%d entity=%d actor=%d foil=%d glint=%d", g_renderFpHooked ? 1 : 0,
            g_hookedEntity ? 1 : 0, g_hookedActor ? 1 : 0, g_hookedFoil ? 1 : 0, g_hookedGlint ? 1 : 0);
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
        else if (key == "opacity")
            g_opacity.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "handOnly")
            g_handOnly.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "outline")
            g_outline.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "galaxy")
            g_galaxy.store(value == "true" || value == "1", std::memory_order_relaxed);
        refreshColor();
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Hand Chams");
    b.description(
         "Solid chams (hand/items + visible players, no wallhack). "
         "Optional dual-pass outline + animated galaxy fill + opacity. "
         "Join world, then enable.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("r", "Red", pl::modmenu::ConfigType::SliderFloat, "0.20", "0", "1", "");
    b.config("g", "Green", pl::modmenu::ConfigType::SliderFloat, "0.90", "0", "1", "");
    b.config("b", "Blue", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("intensity", "Intensity", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.1", "2.0", "");
    b.config("opacity", "Opacity", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.05", "1.0", "");
    b.config("handOnly", "Hand only", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.config("outline", "Outline pass (FP dual)", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("galaxy", "Moving galaxy fill", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.registerModule();
}

void onSignaturesReady() {
    logLine("HandChams: ready — enable after you join the world");
}

void shutdown() { g_enabled.store(false, std::memory_order_release); }

} // namespace bactro::handchams
