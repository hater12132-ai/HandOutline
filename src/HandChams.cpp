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

// Bedrock Color { r, g, b, a }
struct Color {
    float r, g, b, a;
};

constexpr const char* kModuleId = "bactro.handchams";

std::atomic_bool g_enabled{true};
std::atomic<float> g_r{0.15f};
std::atomic<float> g_g{0.85f};
std::atomic<float> g_b{1.00f};
std::atomic<float> g_fill{1.0f};   // 0..1 how hard to replace albedo-ish via change/overlay
std::atomic<float> g_glow{0.35f};  // outer white boost strength
std::atomic_int g_hits{0};

void logLine(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    HC_LOGI("%s", buf);
}

// Visible-model chams only: we never disable depth / never draw hidden geometry.
// Coloring runs on normal engine draws → hand + players you can already see.
bool wantChams() {
    return g_enabled.load(std::memory_order_relaxed);
}

Color makeFill() {
    const float f = g_fill.load(std::memory_order_relaxed);
    return {
        g_r.load(std::memory_order_relaxed) * f,
        g_g.load(std::memory_order_relaxed) * f,
        g_b.load(std::memory_order_relaxed) * f,
        1.0f,
    };
}

Color makeGlowOverlay() {
    // Soft white overlay — engine treats this like hurt/tint; low alpha = thin glow feel
    const float glow = g_glow.load(std::memory_order_relaxed);
    const float a = 0.15f + glow * 0.55f;
    return {1.0f, 1.0f, 1.05f, a};
}

// ---- renderFirstPerson: mark FP hand phase ----
using RenderFirstPersonFn = void (*)(void*, void*, void*, void*, void*, void*);
RenderFirstPersonFn g_renderFpOriginal = nullptr;
bool g_renderFpHooked = false;

void renderFirstPersonDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!g_renderFpOriginal) return;
    bactro::phase::inFirstPersonHand.store(true, std::memory_order_release);
    g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    bactro::phase::inFirstPersonHand.store(false, std::memory_order_release);
}

// ---- setEntityConstants: inject chams colors only in FP hand ----
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

    if (wantChams()) {
        const Color fill = makeFill();
        // Solid-ish fill via changeColor; soft white outer via overlay (glow slider)
        const float glow = g_glow.load(std::memory_order_relaxed);
        const Color overlay = {1.0f, 1.0f, 1.05f, 0.05f + glow * 0.45f};
        g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                             &overlay, &fill, &fill, glintColor, glintUVScale, uvAnim, uvOffset1, uvOffset2,
                             uvRot1, uvRot2);
        const int n = g_hits.fetch_add(1, std::memory_order_relaxed);
        if (n < 12)
            logLine("HandChams: inject #%d rgb=%.2f,%.2f,%.2f glow=%.2f fp=%d", n, fill.r, fill.g, fill.b,
                    glow, bactro::phase::inFirstPersonHand.load() ? 1 : 0);
        return;
    }

    g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                         overlay, changeColor, changeColor2, glintColor, glintUVScale, uvAnim, uvOffset1,
                         uvOffset2, uvRot1, uvRot2);
}

// ---- ActorGlint path (enchanted items in hand) ----
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

    if (wantChams()) {
        const Color fill = makeFill();
        const float glow = g_glow.load(std::memory_order_relaxed);
        const Color overlay = {1.0f, 1.0f, 1.05f, 0.05f + glow * 0.45f};
        g_setupActorGlint(screenContext, entityContext, actor, &overlay, &fill, &fill, glintColor, uvOffset1,
                          uvOffset2, uvRot1, uvRot2, lightEmissionColor);
        return;
    }

    g_setupActorGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor,
                      uvOffset1, uvOffset2, uvRot1, uvRot2, lightEmissionColor);
}

void tryInstallHooks() {
    void* o = nullptr;

    if (!g_renderFpHooked) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ItemInHandRendererRenderFirstPerson,
                                 reinterpret_cast<void*>(&renderFirstPersonDetour), &o)) {
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
                                 reinterpret_cast<void*>(&setEntityConstantsDetour), &o)) {
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
                                 reinterpret_cast<void*>(&setupActorGlintDetour), &o)) {
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
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Hand Chams");
    b.description(
         "First-person hand/held-item solid chams. Configurable RGB + outer glow. "
         "Visual only — does NOT affect other players or world.")
        .defaultEnabled(true)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("r", "Red", pl::modmenu::ConfigType::SliderFloat, "0.15", "0", "1", "");
    b.config("g", "Green", pl::modmenu::ConfigType::SliderFloat, "0.85", "0", "1", "");
    b.config("b", "Blue", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("fill", "Fill strength", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.1", "1.5", "");
    b.config("glow", "Outer glow", pl::modmenu::ConfigType::SliderFloat, "0.35", "0", "1", "");
    b.registerModule();
}

void onSignaturesReady() { tryInstallHooks(); }

void shutdown() {
    g_enabled.store(false, std::memory_order_release);
}

} // namespace bactro::handchams
