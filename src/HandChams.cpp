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
std::atomic_bool g_galaxy{true}; // default on — Phase-style fill
std::atomic<float> g_r{0.20f};
std::atomic<float> g_g{0.90f};
std::atomic<float> g_b{1.00f};
std::atomic<float> g_intensity{1.0f};
std::atomic<float> g_opacity{0.85f};
std::atomic<float> g_stars{0.55f}; // star sparkle amount
std::atomic_int g_hits{0};

Color g_chamsColor{0.12f, 0.14f, 0.45f, 0.85f};

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

// Hash-ish 0..1 from float time buckets (for sparse star blinks)
float hash11(float x) {
    x = std::fmod(x * 0.1031f, 1.0f);
    if (x < 0) x += 1.0f;
    return std::fmod(x * (x + 33.33f) * (x + x), 1.0f);
}

void refreshColor() {
    const float i = g_intensity.load(std::memory_order_relaxed);
    const float a = g_opacity.load(std::memory_order_relaxed);

    if (g_galaxy.load(std::memory_order_relaxed)) {
        const float t = nowSec();

        // Slow drifting nebula (navy / purple / deep blue)
        const float n1 = 0.5f + 0.5f * std::sin(t * 0.55f);
        const float n2 = 0.5f + 0.5f * std::sin(t * 0.92f + 2.1f);
        const float n3 = 0.5f + 0.5f * std::sin(t * 1.25f + 4.0f);

        float r = 0.05f + 0.18f * n2 + 0.12f * n3; // purple bias
        float g = 0.06f + 0.14f * n1 + 0.08f * n2;
        float b = 0.28f + 0.40f * n1 + 0.22f * n2; // deep blue base

        // Moving "stars": sparse bright spikes that blink on/off over time
        // (true spatial stars need a fragment shader; this is mesh-wide sparkle)
        const float starAmt = g_stars.load(std::memory_order_relaxed);
        if (starAmt > 0.01f) {
            // Several layers of blinking at different rates
            for (int k = 0; k < 5; ++k) {
                const float cell = std::floor(t * (2.5f + k * 1.7f) + k * 13.1f);
                const float h = hash11(cell + k * 7.7f);
                if (h > 0.82f) { // rare
                    const float phase = std::fmod(t * (3.0f + k), 1.0f);
                    // sharp twinkle envelope
                    float tw = phase < 0.15f ? (phase / 0.15f) : (phase < 0.35f ? 1.0f : 0.0f);
                    if (phase > 0.35f && phase < 0.5f)
                        tw = 1.0f - (phase - 0.35f) / 0.15f;
                    const float s = tw * starAmt * (0.5f + 0.5f * h);
                    r += s * 0.95f;
                    g += s * 0.95f;
                    b += s * 1.05f;
                }
            }
        }

        // Clamp
        if (r > 1.5f) r = 1.5f;
        if (g > 1.5f) g = 1.5f;
        if (b > 1.5f) b = 1.5f;

        g_chamsColor = {r * i, g * i, b * i, a};
    } else {
        g_chamsColor = {
            g_r.load(std::memory_order_relaxed) * i,
            g_g.load(std::memory_order_relaxed) * i,
            g_b.load(std::memory_order_relaxed) * i,
            a,
        };
    }
}

bool shouldApply() {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    if (g_handOnly.load(std::memory_order_relaxed))
        return bactro::phase::inFirstPersonHand.load(std::memory_order_acquire);
    return true;
}

// ---- FP hand marker only (NO dual-pass — dual white was glitching the fill) ----
using RenderFirstPersonFn = void (*)(void*, void*, void*, void*, void*, void*);
RenderFirstPersonFn g_renderFpOriginal = nullptr;
bool g_renderFpHooked = false;

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

    refreshColor();
    g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                         &g_chamsColor, &g_chamsColor, &g_chamsColor, glintColor, glintUVScale, uvAnim,
                         uvOffset1, uvOffset2, uvRot1, uvRot2);

    const int n = g_hits.fetch_add(1, std::memory_order_relaxed);
    if (n < 6)
        logLine("HandChams: fill #%d rgb=%.2f,%.2f,%.2f a=%.2f galaxy=%d", n, g_chamsColor.r, g_chamsColor.g,
                g_chamsColor.b, g_chamsColor.a, g_galaxy.load() ? 1 : 0);
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
    g_setupActorGlint(screenContext, entityContext, actor, &g_chamsColor, &g_chamsColor, &g_chamsColor,
                      glintColor, uvOffset1, uvOffset2, uvRot1, uvRot2, lightEmissionColor);
}

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
    g_setupFoil(a0, a1, a2, &g_chamsColor, &g_chamsColor, f0, f1, f2, f3);
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
    g_setupGlint(a0, a1, a2, &g_chamsColor, &g_chamsColor, &g_chamsColor, f0, f1, f2, f3);
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
        else if (key == "stars")
            g_stars.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "handOnly")
            g_handOnly.store(value == "true" || value == "1", std::memory_order_relaxed);
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
         "Mesh chams: solid RGB or moving galaxy fill with star sparkles + opacity. "
         "No dual-pass outline (that was washing the hand white). No wallhack. "
         "Join world, then enable.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("galaxy", "Galaxy fill", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("stars", "Star sparkles", pl::modmenu::ConfigType::SliderFloat, "0.55", "0", "1", "");
    b.config("opacity", "Opacity", pl::modmenu::ConfigType::SliderFloat, "0.85", "0.05", "1.0", "");
    b.config("intensity", "Intensity", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.1", "2.0", "");
    b.config("r", "Red (if galaxy off)", pl::modmenu::ConfigType::SliderFloat, "0.20", "0", "1", "");
    b.config("g", "Green (if galaxy off)", pl::modmenu::ConfigType::SliderFloat, "0.90", "0", "1", "");
    b.config("b", "Blue (if galaxy off)", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("handOnly", "Hand only", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.registerModule();
}

void onSignaturesReady() {
    logLine("HandChams: ready — enable after you join the world");
}

void shutdown() { g_enabled.store(false, std::memory_order_release); }

} // namespace bactro::handchams
