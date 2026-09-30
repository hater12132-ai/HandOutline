#include "bactro/MaterialProbe.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <android/log.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#define MP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::material {
namespace {

std::atomic_bool g_enabled{false};
std::atomic_bool g_forceGlow{true};
std::atomic_bool g_dumpFloats{true};
std::atomic_bool g_hooksInstalled{false};

void logLine(const char* fmt, ...) {
    char buf[280];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    MP_LOGI("%s", buf);
}

void dumpFloats(const char* tag, void* p, int count) {
    if (!p || !g_dumpFloats.load(std::memory_order_relaxed)) return;
    auto* f = reinterpret_cast<float*>(p);
    char line[280];
    int n = std::snprintf(line, sizeof(line), "MaterialRE: %s floats", tag);
    for (int i = 0; i < count && n < 240; ++i) {
        float v = f[i];
        if (!std::isfinite(v)) {
            n += std::snprintf(line + n, sizeof(line) - n, " [%d]=nan", i);
            continue;
        }
        if (std::fabs(v) > 1000.f) continue; // skip junk
        n += std::snprintf(line + n, sizeof(line) - n, " [%d]=%.3f", i, v);
    }
    logLine("%s", line);
}

using HideGlowFn = std::int64_t (*)(void*, void*, void*, void*, void*, void*, void*, void*);
HideGlowFn g_hideGlowOrig = nullptr;

std::int64_t hideGlowDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    std::int64_t r = 0;
    if (g_hideGlowOrig) r = g_hideGlowOrig(a0, a1, a2, a3, a4, a5, a6, a7);
    static int s_n = 0;
    if (s_n < 4) {
        logLine("MaterialRE: HideGlow call #%d raw=%lld", s_n, (long long)r);
        ++s_n;
    }
    if (g_enabled.load(std::memory_order_relaxed) && g_forceGlow.load(std::memory_order_relaxed)) return 0;
    return r;
}

using ItemHandFn = void (*)(void*, void*, void*, void*, void*, void*, void*, void*);
ItemHandFn g_itemHandOrig = nullptr;

void itemHandDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    static int s_n = 0;
    const bool en = g_enabled.load(std::memory_order_relaxed);

    // Phase3 write: a1[2]=1.012 a1[3]=0.744 were stable mid-range — try edge boost
    if (en && a1) {
        auto* f = reinterpret_cast<float*>(a1);
        // Only touch if values still look like the observed edge-ish range
        if (std::isfinite(f[2]) && f[2] > 0.2f && f[2] < 3.0f) f[2] = 4.0f; // brightness-ish
        if (std::isfinite(f[3]) && f[3] > 0.1f && f[3] < 2.0f) f[3] = 0.15f; // tightness-ish
        // a0[8] was 1.0 — mild boost
        if (a0) {
            auto* g = reinterpret_cast<float*>(a0);
            if (std::isfinite(g[8]) && g[8] > 0.5f && g[8] < 1.5f) g[8] = 2.0f;
        }
    }

    if (s_n < 6) {
        logLine("MaterialRE: ItemInHandSetup ENTER #%d en=%d", s_n, en ? 1 : 0);
        if (en && a1) {
            auto* f = reinterpret_cast<float*>(a1);
            logLine("MaterialRE: a1 after write [2]=%.3f [3]=%.3f", f[2], f[3]);
        }
        ++s_n;
    }
    if (g_itemHandOrig) g_itemHandOrig(a0, a1, a2, a3, a4, a5, a6, a7);
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
    } else
        logLine("MaterialRE: HideGlowOutlineQuery HOOK FAIL");

    o = nullptr;
    if (bactro::memory::hook(bactro::memory::SignatureId::ItemInHandShaderSetup,
                             reinterpret_cast<void*>(&itemHandDetour), &o) &&
        o) {
        g_itemHandOrig = reinterpret_cast<ItemHandFn>(o);
        logLine("MaterialRE: ItemInHandShaderSetup HOOKED");
        any = true;
    } else
        logLine("MaterialRE: ItemInHandShaderSetup HOOK FAIL");

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
    } else if (key == "dumpFloats") {
        g_dumpFloats.store(value == "true" || value == "1", std::memory_order_relaxed);
        logLine("MaterialRE: dumpFloats=%d", g_dumpFloats.load() ? 1 : 0);
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
    logLine("MaterialRE: EDGE registry @ file 0xdcae78 (ITEM_IN_HAND_EDGE_* ENTITY_EDGE_*)");
    logLine("MaterialRE: phase3 — dump ItemInHand arg floats to map MaterialUniformOverrides");

    static bool reg = false;
    if (!reg) {
        reg = true;
        pl::modmenu::ModuleBuilder b("bactro.material_re", "Material RE Glow");
        b.description("Phase3: dump ItemInHand floats; force HideGlow if called.")
            .defaultEnabled(false)
            .onToggle(onToggle)
            .onConfigChanged(onConfig);
        b.config("forceGlow", "Force show glow", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
        b.config("dumpFloats", "Dump ItemInHand floats", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
        b.registerModule();
        logLine("MaterialRE: module registered");
    }

    if (g_enabled.load(std::memory_order_relaxed)) installHooks();
}

} // namespace bactro::material
