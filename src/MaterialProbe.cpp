#include "bactro/MaterialProbe.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <android/log.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#define MP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::material {
namespace {

std::atomic_bool g_enabled{false};
std::atomic_bool g_forceGlow{true}; // when ON: HideGlowOutline reports false (show glow)
std::atomic_bool g_hooksInstalled{false};

void logLine(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    MP_LOGI("%s", buf);
}

// HideGlowOutline query — (ctx, …) returns non-zero if glow should be hidden
using HideGlowFn = std::int64_t (*)(void*, void*, void*, void*, void*, void*, void*, void*);
HideGlowFn g_hideGlowOrig = nullptr;

std::int64_t hideGlowDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    std::int64_t r = 0;
    if (g_hideGlowOrig) r = g_hideGlowOrig(a0, a1, a2, a3, a4, a5, a6, a7);
    static int s_n = 0;
    if (s_n < 6) {
        logLine("MaterialRE: HideGlow call #%d raw=%lld en=%d", s_n, (long long)r,
                g_enabled.load(std::memory_order_relaxed) ? 1 : 0);
        ++s_n;
    }
    if (g_enabled.load(std::memory_order_relaxed) && g_forceGlow.load(std::memory_order_relaxed)) {
        // Force "do not hide" so engine glow outline path can run
        return 0;
    }
    return r;
}

// Item-in-hand shader setup — wide arity so we do not clobber x3–x7
using ItemHandFn = void (*)(void*, void*, void*, void*, void*, void*, void*, void*);
ItemHandFn g_itemHandOrig = nullptr;

void itemHandDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    static int s_n = 0;
    if (s_n < 6) {
        logLine("MaterialRE: ItemInHandSetup ENTER #%d en=%d", s_n,
                g_enabled.load(std::memory_order_relaxed) ? 1 : 0);
        ++s_n;
    }
    if (g_itemHandOrig) g_itemHandOrig(a0, a1, a2, a3, a4, a5, a6, a7);
}

// Material bin path — log only
using MatBinFn = void (*)(void*, void*, void*, void*, void*, void*, void*, void*, void*);
MatBinFn g_matBinOrig = nullptr;

void matBinDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    static int s_n = 0;
    if (s_n < 3) {
        logLine("MaterialRE: MaterialBinPath ENTER #%d", s_n);
        ++s_n;
    }
    // Forward with best-effort arity — trampoline keeps real registers; we only need log
    if (g_matBinOrig) {
        // Call through as raw function pointer with same stack — use original via casting carefully
        using Raw = void (*)(void*);
        // Prefer storing original and calling with minimal disruption:
        reinterpret_cast<void (*)(void*, void*, void*, void*, void*, void*, void*, void*, void*)>(g_matBinOrig)(
            a0, a1, a2, a3, a4, a5, a6, a7, a8);
    }
}

bool installHooks() {
    if (g_hooksInstalled.load(std::memory_order_acquire)) return true;
    void* o = nullptr;
    bool any = false;

    o = nullptr;
    if (bactro::memory::hook(bactro::memory::SignatureId::HideGlowOutlineQuery,
                             reinterpret_cast<void*>(&hideGlowDetour), &o) &&
        o) {
        g_hideGlowOrig = reinterpret_cast<HideGlowFn>(o);
        logLine("MaterialRE: HideGlowOutlineQuery HOOKED");
        any = true;
    } else {
        logLine("MaterialRE: HideGlowOutlineQuery HOOK FAIL");
    }

    o = nullptr;
    if (bactro::memory::hook(bactro::memory::SignatureId::ItemInHandShaderSetup,
                             reinterpret_cast<void*>(&itemHandDetour), &o) &&
        o) {
        g_itemHandOrig = reinterpret_cast<ItemHandFn>(o);
        logLine("MaterialRE: ItemInHandShaderSetup HOOKED");
        any = true;
    } else {
        logLine("MaterialRE: ItemInHandShaderSetup HOOK FAIL");
    }

    // Skip MaterialBinPathBuilder hook — arity unknown; resolve-only is enough for now

    g_hooksInstalled.store(any, std::memory_order_release);
    return any;
}

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    logLine("MaterialRE %s", enabled ? "ON" : "OFF");
    if (enabled) installHooks();
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    if (key == "forceGlow") {
        g_forceGlow.store(value == "true" || value == "1", std::memory_order_relaxed);
        logLine("MaterialRE: forceGlow=%d", g_forceGlow.load() ? 1 : 0);
    }
}

} // namespace

void onSignaturesReady() {
    using bactro::memory::SignatureId;
    using bactro::memory::resolve;

    const auto hide = resolve(SignatureId::HideGlowOutlineQuery);
    const auto hand = resolve(SignatureId::ItemInHandShaderSetup);
    const auto mbin = resolve(SignatureId::MaterialBinPathBuilder);
    const auto miss = resolve(SignatureId::MaterialMissingError);
    const auto group = resolve(SignatureId::RenderMaterialGroupCommon);

    logLine("MaterialRE: HideGlowOutlineQuery %s @%p", hide ? "OK" : "MISS", reinterpret_cast<void*>(hide));
    logLine("MaterialRE: ItemInHandShaderSetup %s @%p", hand ? "OK" : "MISS", reinterpret_cast<void*>(hand));
    logLine("MaterialRE: MaterialBinPathBuilder %s @%p", mbin ? "OK" : "MISS", reinterpret_cast<void*>(mbin));
    logLine("MaterialRE: MaterialMissingError %s @%p", miss ? "OK" : "MISS", reinterpret_cast<void*>(miss));
    logLine("MaterialRE: RenderMaterialGroupCommon %s @%p", group ? "OK" : "MISS", reinterpret_cast<void*>(group));
    logLine("MaterialRE: EDGE uniforms: ITEM_IN_HAND_EDGE_* ENTITY_EDGE_*");

    // Register module once
    static bool reg = false;
    if (!reg) {
        reg = true;
        pl::modmenu::ModuleBuilder b("bactro.material_re", "Material RE Glow");
        b.description("Native RD: force HideGlowOutline off + ItemInHand setup probe. Phase2.")
            .defaultEnabled(false)
            .onToggle(onToggle)
            .onConfigChanged(onConfig);
        b.config("forceGlow", "Force show glow (HideGlow=0)", pl::modmenu::ConfigType::Toggle, "true", "", "",
                 "");
        b.registerModule();
        logLine("MaterialRE: module registered");
    }

    if (g_enabled.load(std::memory_order_relaxed)) installHooks();
}

} // namespace bactro::material
