#include "bactro/MaterialProbe.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include "bactro/Guard.hpp"
#include "bactro/RenderPhase.hpp"

#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <android/log.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>

#define MP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::material {
namespace {

void logLine(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    MP_LOGI("%s", buf);
}


// ---- Safe memory read (never faults): write() to a pipe returns EFAULT on a bad address ----
int g_pipe[2] = {-1, -1};
bool safeRead(const void* p, void* out, size_t n) {
    if (!p || n == 0 || n > 4096) return false;
    if (g_pipe[0] < 0) {
        if (pipe2(g_pipe, O_NONBLOCK | O_CLOEXEC) != 0) return false;
    }
    const ssize_t w = write(g_pipe[1], p, n);
    if (w != static_cast<ssize_t>(n)) {
        if (w > 0) { char d[4096]; (void)read(g_pipe[0], d, static_cast<size_t>(w)); }
        return false;
    }
    return read(g_pipe[0], out, n) == static_cast<ssize_t>(n);
}

// Longest printable-ASCII run (>= 4 chars) inside buf; copies it to out (NUL-terminated).
bool asciiRun(const unsigned char* buf, size_t n, char* out, size_t outCap) {
    size_t bestLen = 0, bestPos = 0, i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && buf[j] >= 0x20 && buf[j] < 0x7f) ++j;
        if (j - i > bestLen) { bestLen = j - i; bestPos = i; }
        i = (j == i) ? i + 1 : j;
    }
    if (bestLen < 4) return false;
    if (bestLen > outCap - 1) bestLen = outCap - 1;
    std::memcpy(out, buf + bestPos, bestLen);
    out[bestLen] = 0;
    return true;
}

// Try to pull a readable string out of a pointer-ish argument: raw bytes, libc++ std::string,
// {ptr,len} view, or one pointer deref. Pure heuristics, all reads are fault-safe.
bool describeArg(const void* arg, char* out, size_t cap) {
    unsigned char b[64];
    if (!safeRead(arg, b, sizeof(b))) return false;
    if (asciiRun(b, sizeof(b), out, cap)) return true;           // inline chars / SSO string
    for (int off = 0; off < 32; off += 8) {                       // pointer fields (long string, view)
        std::uintptr_t q;
        std::memcpy(&q, b + off, 8);
        if (q < 0x10000) continue;
        unsigned char c[96];
        if (safeRead(reinterpret_cast<const void*>(q), c, sizeof(c)) && asciiRun(c, sizeof(c), out, cap)) return true;
    }
    return false;
}

// ---- 1) AAssetManager_open logger (public NDK ABI: AAsset* f(AAssetManager*, const char*, int)) ----
std::atomic_bool g_assetOn{false};
bool g_assetHooked = false;
void* (*g_assetOrig)(void*, const char*, int) = nullptr;
std::atomic<int> g_assetLines{0};

void* assetOpenDetour(void* mgr, const char* name, int mode) {
    if (g_assetOn.load(std::memory_order_relaxed) && name) {
        char buf[200];
        if (safeRead(name, buf, 120)) {
            buf[119] = 0;
            if (std::strstr(buf, "material") || std::strstr(buf, "renderer")) {
                const int n = g_assetLines.fetch_add(1, std::memory_order_relaxed);
                if (n < 250) logLine("AssetProbe: open \"%.150s\" mode=%d", buf, mode);
            }
        }
    }
    return g_assetOrig ? g_assetOrig(mgr, name, mode) : nullptr;
}

bool installAssetHook() {
    if (g_assetHooked) return true;
    void* h = dlopen("libandroid.so", RTLD_NOW);
    void* fn = h ? dlsym(h, "AAssetManager_open") : nullptr;
    if (!fn) { logLine("AssetProbe: AAssetManager_open not found"); return false; }
    void* orig = nullptr;
    if (pl::memory::hook(fn, reinterpret_cast<void*>(&assetOpenDetour), &orig) != 0 || !orig) {
        logLine("AssetProbe: hook FAIL");
        return false;
    }
    g_assetOrig = reinterpret_cast<void* (*)(void*, const char*, int)>(orig);
    g_assetHooked = true;
    logLine("AssetProbe: AAssetManager_open hooked (pass-through)");
    return true;
}

// ---- 2) MaterialBinPathBuilder logger ----
// 1.26.52 prologue: `mov x19, x8` then `str w9,[x19,#0x1b0]` => the function RETURNS A BIG OBJECT through x8
// (indirect result, >= 0x1b4 bytes). A plain void detour would drop x8 and the original would write through
// garbage. Declaring the detour with the same big return type makes the compiler pass x8 along, and
// `return orig(...)` forwards it without copying.
using U64 = std::uint64_t;
struct MbpRet {
    alignas(16) unsigned char raw[0x400];
};
using MbpFn = MbpRet (*)(void*, void*, void*, void*, void*, void*, void*, void*, double, double, double, double,
                         double, double, double, double, U64, U64, U64, U64, U64, U64, U64, U64);
std::atomic_bool g_pathOn{false};
bool g_pathHooked = false;
MbpFn g_pathOrig = nullptr;
std::atomic<int> g_pathLines{0};

MbpRet pathDetour(void* x0, void* x1, void* x2, void* x3, void* x4, void* x5, void* x6, void* x7, double d0,
                  double d1, double d2, double d3, double d4, double d5, double d6, double d7, U64 t0, U64 t1,
                  U64 t2, U64 t3, U64 t4, U64 t5, U64 t6, U64 t7) {
    if (g_pathOn.load(std::memory_order_relaxed)) {
        const int n = g_pathLines.fetch_add(1, std::memory_order_relaxed);
        if (n < 120) {
            char s0[100] = "-", s1[100] = "-", s2[100] = "-", s3[100] = "-";
            describeArg(x0, s0, sizeof(s0));
            describeArg(x1, s1, sizeof(s1));
            describeArg(x2, s2, sizeof(s2));
            describeArg(x3, s3, sizeof(s3));
            logLine("PathProbe#%d x1=%p [%.60s] x2=%p [%.50s]", n, x1, s1, x2, s2);
            if (n < 20) logLine("PathProbe#%d x0=%p [%.60s] x3=%p [%.50s]", n, x0, s0, x3, s3);
        }
    }
    return g_pathOrig(x0, x1, x2, x3, x4, x5, x6, x7, d0, d1, d2, d3, d4, d5, d6, d7, t0, t1, t2, t3, t4, t5, t6, t7);
}

bool installPathHook() {
    if (g_pathHooked) return true;
    void* o = nullptr;
    if (!bactro::memory::hook(bactro::memory::SignatureId::MaterialBinPathBuilder,
                              reinterpret_cast<void*>(&pathDetour), &o) || !o) {
        logLine("PathProbe: hook FAIL / signature missing");
        return false;
    }
    g_pathOrig = reinterpret_cast<MbpFn>(o);
    g_pathHooked = true;
    logLine("PathProbe: MaterialBinPathBuilder hooked (x8-safe pass-through)");
    return true;
}

} // namespace

void onSignaturesReady() {
    using bactro::memory::SignatureId;
    using bactro::memory::resolve;

    // Resolve-only — no hooks, no float writes (1.16.5 a1 write was wrong object)
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
    const auto matreg = resolve(SignatureId::MaterialRegistryInit);
    logLine("MaterialRE: MaterialRegistryInit (item_in_hand_glint+flat_color_line) %s @%p", matreg ? "OK" : "MISS", reinterpret_cast<void*>(matreg));
    logLine("MaterialRE: STOPPED experimental writes — a1 was not EDGE uniforms");
    logLine("MaterialRE: need MaterialFilter / custom material.bin bind for real chams");
}

void setAssetProbe(bool on) {
    if (bactro::phase::safeMode.load(std::memory_order_acquire)) { logLine("AssetProbe: skipped (SAFE MODE)"); return; }
    if (on && !installAssetHook()) return;
    g_assetOn.store(on, std::memory_order_relaxed);
    logLine(on ? "AssetProbe ON - open a world, wait a few seconds, then turn it off" : "AssetProbe OFF");
}

void setPathProbe(bool on) {
    if (bactro::phase::safeMode.load(std::memory_order_acquire)) { logLine("PathProbe: skipped (SAFE MODE)"); return; }
    if (on) {
        bactro::guard::noteRiskyHookRan();
        if (!installPathHook()) return;
    }
    g_pathOn.store(on, std::memory_order_relaxed);
    logLine(on ? "PathProbe ON" : "PathProbe OFF");
}

} // namespace bactro::material
