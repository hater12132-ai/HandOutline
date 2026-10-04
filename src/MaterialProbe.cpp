#include "bactro/MaterialProbe.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include "bactro/Guard.hpp"
#include "bactro/RenderPhase.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>
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

// ---- 1) Native asset interception (public NDK ABI, no resource pack, no extra loader mod) ----
// Findings from your log: the game loads "assets/renderer/materials/<Name>.material.bin" through
// AAssetManager_open(mode=3 / buffer). Mobs and players use Actor.material.bin.
//   * Asset probe   : log material opens.
//   * Material dump : save the ORIGINAL Actor/Entity/ItemInHand material.bin to <root>/dump/ (so it can be analysed).
//   * Override      : if <root>/override/<Name>.material.bin exists, serve that file instead of the APK's.
constexpr const char* kRoot = "/storage/emulated/0/Android/media/org.levimc.launcher/bactro_materials";

std::atomic_bool g_assetOn{false};
std::atomic_bool g_dumpOn{false};
std::atomic_bool g_ovOn{false};
bool g_assetHooked = false;
bool g_ovHooked = false;
using OpenFn = void* (*)(void*, const char*, int);
OpenFn g_assetOrig = nullptr;
std::atomic<int> g_assetLines{0};

// Real libandroid entry points (resolved with dlsym; calling them is always safe).
int64_t (*f_getLength64)(void*) = nullptr;
const void* (*f_getBuffer)(void*) = nullptr;

// Originals returned by the hook engine (override hooks only).
int64_t (*o_getLength)(void*) = nullptr;
int64_t (*o_getLength64)(void*) = nullptr;
int64_t (*o_getRemaining)(void*) = nullptr;
int64_t (*o_getRemaining64)(void*) = nullptr;
const void* (*o_getBuffer)(void*) = nullptr;
int (*o_read)(void*, void*, size_t) = nullptr;
int64_t (*o_seek)(void*, int64_t, int) = nullptr;
int64_t (*o_seek64)(void*, int64_t, int) = nullptr;
void (*o_close)(void*) = nullptr;
int (*o_isAllocated)(void*) = nullptr;
int (*o_openFd)(void*, int64_t*, int64_t*) = nullptr;
int (*o_openFd64)(void*, int64_t*, int64_t*) = nullptr;

struct Ov {
    std::vector<unsigned char> data;
    int64_t pos = 0;
};
std::mutex g_ovMu;
std::unordered_map<void*, Ov> g_ov;
std::atomic<int> g_ovCount{0};

#define OV_FIND(a)                                      \
    if (g_ovCount.load(std::memory_order_relaxed) > 0) { \
        std::lock_guard<std::mutex> lk(g_ovMu);          \
        auto it = g_ov.find(a);                          \
        if (it != g_ov.end()) {
#define OV_END \
        }      \
    }

int64_t dGetLength(void* a) {
    OV_FIND(a) return static_cast<int64_t>(it->second.data.size());
    OV_END
    return o_getLength(a);
}
int64_t dGetLength64(void* a) {
    OV_FIND(a) return static_cast<int64_t>(it->second.data.size());
    OV_END
    return o_getLength64(a);
}
int64_t dGetRemaining(void* a) {
    OV_FIND(a) return static_cast<int64_t>(it->second.data.size()) - it->second.pos;
    OV_END
    return o_getRemaining(a);
}
int64_t dGetRemaining64(void* a) {
    OV_FIND(a) return static_cast<int64_t>(it->second.data.size()) - it->second.pos;
    OV_END
    return o_getRemaining64(a);
}
const void* dGetBuffer(void* a) {
    OV_FIND(a) return it->second.data.data();
    OV_END
    return o_getBuffer(a);
}
int dRead(void* a, void* dst, size_t n) {
    OV_FIND(a) {
        const int64_t size = static_cast<int64_t>(it->second.data.size());
        int64_t left = size - it->second.pos;
        if (left < 0) left = 0;
        const size_t take = static_cast<size_t>(left) < n ? static_cast<size_t>(left) : n;
        if (take) std::memcpy(dst, it->second.data.data() + it->second.pos, take);
        it->second.pos += static_cast<int64_t>(take);
        return static_cast<int>(take);
    }
    OV_END
    return o_read(a, dst, n);
}
int64_t seekImpl(Ov& o, int64_t off, int whence) {
    const int64_t size = static_cast<int64_t>(o.data.size());
    int64_t np = 0;
    if (whence == SEEK_SET) np = off;
    else if (whence == SEEK_CUR) np = o.pos + off;
    else if (whence == SEEK_END) np = size + off;
    else return -1;
    if (np < 0 || np > size) return -1;
    o.pos = np;
    return np;
}
int64_t dSeek(void* a, int64_t off, int whence) {
    OV_FIND(a) return seekImpl(it->second, off, whence);
    OV_END
    return o_seek(a, off, whence);
}
int64_t dSeek64(void* a, int64_t off, int whence) {
    OV_FIND(a) return seekImpl(it->second, off, whence);
    OV_END
    return o_seek64(a, off, whence);
}
void dClose(void* a) {
    if (g_ovCount.load(std::memory_order_relaxed) > 0) {
        std::lock_guard<std::mutex> lk(g_ovMu);
        if (g_ov.erase(a)) g_ovCount.fetch_sub(1, std::memory_order_relaxed);
    }
    o_close(a);
}
int dIsAllocated(void* a) {
    OV_FIND(a) return 1;
    OV_END
    return o_isAllocated(a);
}
int dOpenFd(void* a, int64_t* st, int64_t* ln) {
    OV_FIND(a) return -1;
    OV_END
    return o_openFd(a, st, ln);
}
int dOpenFd64(void* a, int64_t* st, int64_t* ln) {
    OV_FIND(a) return -1;
    OV_END
    return o_openFd64(a, st, ln);
}
#undef OV_FIND
#undef OV_END

// Some libandroid exports (e.g. AAsset_getLength / AAsset_getLength64) can be folded onto one address.
// Hooking the same address twice is not safe, so the second symbol just reuses the first original; the
// paired detours behave identically for tracked assets, so the first detour covers both.
bool hookSym(void* lib, const char* sym, void* detour, void** orig) {
    static std::unordered_map<void*, void*> s_done;
    void* fn = dlsym(lib, sym);
    if (!fn) return false;
    auto it = s_done.find(fn);
    if (it != s_done.end()) {
        *orig = it->second;
        return *orig != nullptr;
    }
    if (pl::memory::hook(fn, detour, orig) != 0 || !*orig) return false;
    s_done[fn] = *orig;
    return true;
}

bool installOverrideHooks() {
    if (g_ovHooked) return true;
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) { logLine("MatOverride: libandroid.so not found"); return false; }
    bool ok = true;
#define HK(sym, det, orig) ok = hookSym(lib, sym, reinterpret_cast<void*>(&det), reinterpret_cast<void**>(&orig)) && ok
    HK("AAsset_getLength", dGetLength, o_getLength);
    HK("AAsset_getLength64", dGetLength64, o_getLength64);
    HK("AAsset_getRemainingLength", dGetRemaining, o_getRemaining);
    HK("AAsset_getRemainingLength64", dGetRemaining64, o_getRemaining64);
    HK("AAsset_getBuffer", dGetBuffer, o_getBuffer);
    HK("AAsset_read", dRead, o_read);
    HK("AAsset_seek", dSeek, o_seek);
    HK("AAsset_seek64", dSeek64, o_seek64);
    HK("AAsset_close", dClose, o_close);
    HK("AAsset_isAllocated", dIsAllocated, o_isAllocated);
    HK("AAsset_openFileDescriptor", dOpenFd, o_openFd);
    HK("AAsset_openFileDescriptor64", dOpenFd64, o_openFd64);
#undef HK
    if (!ok) {
        logLine("MatOverride: some AAsset_* hooks failed - override disabled");
        return false;
    }
    g_ovHooked = true;
    logLine("MatOverride: AAsset_* hooks installed");
    return true;
}

void ensureDirs() {
    mkdir(kRoot, 0777);
    mkdir((std::string(kRoot) + "/dump").c_str(), 0777);
    mkdir((std::string(kRoot) + "/override").c_str(), 0777);
}

bool wantDump(const std::string& f) {
    return f.find("Actor") != std::string::npos || f.find("Entity") != std::string::npos ||
           f.find("ItemInHand") != std::string::npos;
}

void maybeDump(void* asset, const std::string& fname) {
    static std::mutex m;
    static std::set<std::string> done;
    {
        std::lock_guard<std::mutex> lk(m);
        if (!done.insert(fname).second) return;
    }
    if (!f_getLength64 || !f_getBuffer) return;
    const int64_t len = f_getLength64(asset);
    const void* buf = f_getBuffer(asset);
    if (len <= 0 || !buf) {
        logLine("MatDump: %.60s unreadable (len=%lld)", fname.c_str(), static_cast<long long>(len));
        return;
    }
    ensureDirs();
    const std::string path = std::string(kRoot) + "/dump/" + fname;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { logLine("MatDump: cannot write %.80s", path.c_str()); return; }
    const size_t w = std::fwrite(buf, 1, static_cast<size_t>(len), f);
    std::fclose(f);
    logLine("MatDump: saved %.60s (%lld bytes, wrote %lld)", fname.c_str(), static_cast<long long>(len),
            static_cast<long long>(w));
}

void maybeOverride(void* asset, const std::string& fname) {
    const std::string path = std::string(kRoot) + "/override/" + fname;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    std::vector<unsigned char> data;
    unsigned char tmp[16384];
    size_t n;
    while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0) data.insert(data.end(), tmp, tmp + n);
    std::fclose(f);
    if (data.empty()) return;
    const size_t sz = data.size();
    {
        std::lock_guard<std::mutex> lk(g_ovMu);
        Ov o;
        o.data = std::move(data);
        g_ov[asset] = std::move(o);
    }
    g_ovCount.fetch_add(1, std::memory_order_relaxed);
    static int s_ovLog = 0;
    if (s_ovLog < 20) { ++s_ovLog; logLine("MatOverride: serving %.60s from override (%lld bytes)", fname.c_str(), static_cast<long long>(sz)); }
}

bool readName(const char* name, char* out, size_t cap) {
    static const size_t tries[] = {120, 64, 32, 16};
    for (size_t t : tries) {
        if (t >= cap) continue;
        if (safeRead(name, out, t)) { out[t] = 0; out[cap - 1] = 0; return true; }
    }
    return false;
}

void* assetOpenDetour(void* mgr, const char* name, int mode) {
    void* a = g_assetOrig ? g_assetOrig(mgr, name, mode) : nullptr;
    if (!a || !name) return a;
    if (!g_assetOn.load(std::memory_order_relaxed) && !g_dumpOn.load(std::memory_order_relaxed) &&
        !g_ovOn.load(std::memory_order_relaxed))
        return a;
    char buf[200] = {0};
    if (!readName(name, buf, sizeof(buf))) return a;
    if (!std::strstr(buf, "material") && !std::strstr(buf, "renderer")) return a;

    if (g_assetOn.load(std::memory_order_relaxed)) {
        const int n = g_assetLines.fetch_add(1, std::memory_order_relaxed);
        if (n < 250) logLine("AssetProbe: open \"%.150s\" mode=%d", buf, mode);
    }
    if (std::strstr(buf, ".material.bin")) {
        const char* slash = std::strrchr(buf, '/');
        const std::string fname = slash ? slash + 1 : buf;
        if (g_dumpOn.load(std::memory_order_relaxed) && wantDump(fname)) maybeDump(a, fname);
        if (g_ovOn.load(std::memory_order_relaxed) && g_ovHooked) maybeOverride(a, fname);
    }
    return a;
}

bool installAssetHook() {
    if (g_assetHooked) return true;
    void* h = dlopen("libandroid.so", RTLD_NOW);
    void* fn = h ? dlsym(h, "AAssetManager_open") : nullptr;
    if (!fn) { logLine("AssetProbe: AAssetManager_open not found"); return false; }
    f_getLength64 = reinterpret_cast<int64_t (*)(void*)>(dlsym(h, "AAsset_getLength64"));
    f_getBuffer = reinterpret_cast<const void* (*)(void*)>(dlsym(h, "AAsset_getBuffer"));
    void* orig = nullptr;
    if (pl::memory::hook(fn, reinterpret_cast<void*>(&assetOpenDetour), &orig) != 0 || !orig) {
        logLine("AssetProbe: hook FAIL");
        return false;
    }
    g_assetOrig = reinterpret_cast<OpenFn>(orig);
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
    logLine(on ? "AssetProbe ON - leave and re-join the world, then turn it off" : "AssetProbe OFF");
}

void setMaterialDump(bool on) {
    if (bactro::phase::safeMode.load(std::memory_order_acquire)) { logLine("MatDump: skipped (SAFE MODE)"); return; }
    if (on && !installAssetHook()) return;
    g_dumpOn.store(on, std::memory_order_relaxed);
    logLine(on ? "MatDump ON - leave and re-join the world; files go to bactro_materials/dump" : "MatDump OFF");
}

void setMaterialOverride(bool on) {
    if (bactro::phase::safeMode.load(std::memory_order_acquire)) { logLine("MatOverride: skipped (SAFE MODE)"); return; }
    if (on) {
        bactro::guard::noteRiskyHookRan();
        if (!installAssetHook() || !installOverrideHooks()) return;
        ensureDirs();
    }
    g_ovOn.store(on, std::memory_order_relaxed);
    logLine(on ? "MatOverride ON - serving bactro_materials/override/*.material.bin on next load" : "MatOverride OFF");
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
