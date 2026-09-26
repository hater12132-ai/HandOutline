#include "bactro/WireOutline.hpp"
#include "bactro/RenderPhase.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <EGL/egl.h>
#include <android/log.h>
#include <dlfcn.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define WO_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::wireoutline {
namespace {

constexpr const char* kModuleId = "bactro.wireoutline";

std::atomic_bool g_enabled{false};
std::atomic_bool g_handItems{true};
std::atomic_bool g_armor{true};
std::atomic_bool g_players{true};
std::atomic_bool g_crystals{true};
std::atomic_bool g_draw3d{true};
std::atomic<float> g_lineWidth{2.0f};

using ActorIsPlayerFn = bool (*)(void*);
using HitResultGetEntityFn = void* (*)(void*);
ActorIsPlayerFn g_isPlayer = nullptr;
HitResultGetEntityFn g_hitGetEntity = nullptr;
std::uintptr_t g_levelGetHitResult = 0;
std::uintptr_t g_fetchNearby = 0;
std::uintptr_t g_clientGetLocalPlayer = 0;

void* g_localPlayer = nullptr; // set from NormalTick when isPlayer
std::mutex g_boxMu;
struct Box {
    float minx, miny, minz, maxx, maxy, maxz;
};
std::vector<Box> g_boxes; // world-space AABBs to draw this frame

void logLine(const char* fmt, ...) {
    char buf[220];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    WO_LOGI("%s", buf);
}

bool finite3(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

// Reasonable overworld-ish coords for 1.26
bool looksLikeWorldPos(float x, float y, float z) {
    if (!finite3(x, y, z)) return false;
    if (std::fabs(x) > 300000.f || std::fabs(z) > 300000.f) return false;
    if (y < -80.f || y > 400.f) return false;
    // reject near-zero junk
    if (std::fabs(x) < 1e-3f && std::fabs(y) < 1e-3f && std::fabs(z) < 1e-3f) return false;
    return true;
}

bool looksLikeAABB(float minx, float miny, float minz, float maxx, float maxy, float maxz) {
    if (!finite3(minx, miny, minz) || !finite3(maxx, maxy, maxz)) return false;
    if (maxx <= minx || maxy <= miny || maxz <= minz) return false;
    const float dx = maxx - minx, dy = maxy - miny, dz = maxz - minz;
    if (dx < 0.05f || dy < 0.05f || dz < 0.05f) return false;
    if (dx > 8.f || dy > 12.f || dz > 8.f) return false; // entity-sized
    if (!looksLikeWorldPos((minx + maxx) * 0.5f, (miny + maxy) * 0.5f, (minz + maxz) * 0.5f))
        return false;
    return true;
}

// ---- Probe Actor memory for AABBShapeComponent-like data (min,max or pos+size) ----
// Public SDK: AABBShape { Vec3 min, Vec3 max, float w, float h } @ ~0x20
// StateVector { Vec3 pos, prev, delta } @ ~0x24
// We only ACCEPT values that pass looksLike* — never invent.
bool probeActorBox(void* actor, Box& out) {
    if (!actor) return false;
    auto* base = reinterpret_cast<unsigned char*>(actor);

    // Try a small set of offsets used by various 1.20–1.21 dumps (validated by looksLike only)
    static const int kPosOff[] = {0x48, 0x50, 0x68, 0x70, 0x88, 0x90, 0xA0, 0xB0, 0xC8, 0xD0,
                                  0x100, 0x108, 0x120, 0x128, 0x148, 0x150, 0x168, 0x190,
                                  0x1A0, 0x1C0, 0x1E0, 0x200, 0x220, 0x240, 0x280, 0x2A0};
    for (int off : kPosOff) {
        auto* f = reinterpret_cast<float*>(base + off);
        // Pattern A: AABB min/max (6 floats)
        if (looksLikeAABB(f[0], f[1], f[2], f[3], f[4], f[5])) {
            out = {f[0], f[1], f[2], f[3], f[4], f[5]};
            return true;
        }
        // Pattern B: pos + default player size (0.6 x 1.8 x 0.6)
        if (looksLikeWorldPos(f[0], f[1], f[2])) {
            const float x = f[0], y = f[1], z = f[2];
            const float hw = 0.3f, h = 1.8f;
            out = {x - hw, y, z - hw, x + hw, y + h, z + hw};
            return true;
        }
    }
    return false;
}

// ---- NormalTick: capture local player ----
using NormalTickFn = void (*)(void*);
NormalTickFn g_tickOriginal = nullptr;
bool g_tickHooked = false;

void normalTickDetour(void* self) {
    if (g_tickOriginal) g_tickOriginal(self);
    if (!g_enabled.load(std::memory_order_relaxed) || !self) return;
    if (g_isPlayer) {
        bool ok = false;
        try {
            ok = g_isPlayer(self);
        } catch (...) {
            ok = false;
        }
        if (ok) g_localPlayer = self;
    }
}

// ---- FP hand marker ----
using RenderFirstPersonFn = void (*)(void*, void*, void*, void*, void*, void*);
RenderFirstPersonFn g_renderFpOriginal = nullptr;
bool g_fpHooked = false;

void renderFirstPersonDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!g_renderFpOriginal) return;
    const bool want = g_enabled.load(std::memory_order_relaxed) && g_handItems.load(std::memory_order_relaxed);
    if (want) bactro::phase::inFirstPersonHand.store(true, std::memory_order_release);
    g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    if (want) bactro::phase::inFirstPersonHand.store(false, std::memory_order_release);
}

// Collect boxes once per frame from local player + hit entity
void collectBoxes() {
    std::vector<Box> next;
    void* lp = g_localPlayer;
    if (lp && g_players.load(std::memory_order_relaxed)) {
        // don't outline self by default — skip local; other players need nearby list
        (void)lp;
    }

    // Hit-result entity (whatever you're looking at)
    // LevelGetHitResult needs Level* — without ClientInstance->level we skip call.
    // Instead: if we previously stored hit entity another way, use probe.

    // Probe local player only for debug box (optional): proves probe works in-world
    // Disabled for self to avoid messy self-outline; enable via players+debug later.

    {
        std::lock_guard<std::mutex> lock(g_boxMu);
        g_boxes.swap(next);
    }
}

// ---- GLES line renderer (NDC after CPU project) ----
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLfloat = float;
using GLchar = char;
using GLsizei = int;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_LINES = 0x0001;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;

using PFN_void = void (*)();
void* glProc(const char* name) {
    if (void* p = reinterpret_cast<void*>(eglGetProcAddress(name))) return p;
    void* h = dlopen("libGLESv2.so", RTLD_NOW);
    if (!h) h = dlopen("libGLESv3.so", RTLD_NOW);
    return h ? dlsym(h, name) : nullptr;
}

#define GLDECL(ret, name, ...)           \
    using PFN_##name = ret (*)(__VA_ARGS__); \
    PFN_##name p_##name = nullptr

GLDECL(GLuint, glCreateShader, GLenum);
GLDECL(void, glShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*);
GLDECL(void, glCompileShader, GLuint);
GLDECL(void, glGetShaderiv, GLuint, GLenum, GLint*);
GLDECL(GLuint, glCreateProgram);
GLDECL(void, glAttachShader, GLuint, GLuint);
GLDECL(void, glLinkProgram, GLuint);
GLDECL(void, glGetProgramiv, GLuint, GLenum, GLint*);
GLDECL(void, glUseProgram, GLuint);
GLDECL(GLint, glGetAttribLocation, GLuint, const GLchar*);
GLDECL(GLint, glGetUniformLocation, GLuint, const GLchar*);
GLDECL(void, glUniform4f, GLint, GLfloat, GLfloat, GLfloat, GLfloat);
GLDECL(void, glGenBuffers, GLsizei, GLuint*);
GLDECL(void, glBindBuffer, GLenum, GLuint);
GLDECL(void, glBufferData, GLenum, long, const void*, GLenum);
GLDECL(void, glEnableVertexAttribArray, GLuint);
GLDECL(void, glVertexAttribPointer, GLuint, GLint, GLenum, unsigned char, GLsizei, const void*);
GLDECL(void, glDrawArrays, GLenum, GLint, GLsizei);
GLDECL(void, glEnable, GLenum);
GLDECL(void, glDisable, GLenum);
GLDECL(void, glBlendFunc, GLenum, GLenum);
GLDECL(void, glLineWidth, GLfloat);
GLDECL(void, glDeleteShader, GLuint);

bool g_glReady = false;
GLuint g_prog = 0;
GLuint g_vbo = 0;
GLint g_aPos = -1;
GLint g_uColor = -1;
std::mutex g_glMu;
int g_drawLog = 0;

bool loadGl() {
    if (g_glReady) return g_prog != 0;
    g_glReady = true;
#define L(n) p_##n = reinterpret_cast<PFN_##n>(glProc(#n))
    L(glCreateShader);
    L(glShaderSource);
    L(glCompileShader);
    L(glGetShaderiv);
    L(glCreateProgram);
    L(glAttachShader);
    L(glLinkProgram);
    L(glGetProgramiv);
    L(glUseProgram);
    L(glGetAttribLocation);
    L(glGetUniformLocation);
    L(glUniform4f);
    L(glGenBuffers);
    L(glBindBuffer);
    L(glBufferData);
    L(glEnableVertexAttribArray);
    L(glVertexAttribPointer);
    L(glDrawArrays);
    L(glEnable);
    L(glDisable);
    L(glBlendFunc);
    L(glLineWidth);
    L(glDeleteShader);
#undef L
    if (!p_glCreateShader || !p_glDrawArrays) {
        logLine("WireOutline: GLES load FAIL");
        return false;
    }
    const char* vs = "attribute vec2 aPos; void main(){ gl_Position=vec4(aPos,0.0,1.0); }";
    const char* fs = "precision mediump float; uniform vec4 uColor; void main(){ gl_FragColor=uColor; }";
    auto compile = [](GLenum t, const char* s) -> GLuint {
        GLuint sh = p_glCreateShader(t);
        p_glShaderSource(sh, 1, &s, nullptr);
        p_glCompileShader(sh);
        GLint ok = 0;
        p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        return ok ? sh : 0;
    };
    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) {
        logLine("WireOutline: shader compile FAIL");
        return false;
    }
    g_prog = p_glCreateProgram();
    p_glAttachShader(g_prog, v);
    p_glAttachShader(g_prog, f);
    p_glLinkProgram(g_prog);
    GLint linked = 0;
    p_glGetProgramiv(g_prog, GL_LINK_STATUS, &linked);
    p_glDeleteShader(v);
    p_glDeleteShader(f);
    if (!linked) {
        g_prog = 0;
        logLine("WireOutline: shader link FAIL");
        return false;
    }
    g_aPos = p_glGetAttribLocation(g_prog, "aPos");
    g_uColor = p_glGetUniformLocation(g_prog, "uColor");
    p_glGenBuffers(1, &g_vbo);
    logLine("WireOutline: GLES lines ready");
    return true;
}

// Simple perspective from local player-ish camera (yaw/pitch unknown → use identity view).
// Until real CameraAPI matrix is found we only project if we also have local player pos
// and treat camera at local feet+1.6 looking +Z — WRONG for real play.
// So: **do not draw** until we have a better matrix. This function stays ready.
void projectAndDraw(const std::vector<Box>& boxes) {
    if (boxes.empty()) return;
    if (!g_draw3d.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_glMu);
    if (!loadGl() || !g_prog) return;

    // Without camera matrix, skip draw (prevents wrong boxes / crashes).
    // Status once:
    if (g_drawLog < 3) {
        logLine("WireOutline: have %d boxes but camera matrix not locked — no draw", (int)boxes.size());
        ++g_drawLog;
    }
    (void)boxes;
}

void resolveAndHook() {
    if (!g_isPlayer) {
        auto a = bactro::memory::resolve(bactro::memory::SignatureId::ActorIsPlayer);
        if (a) {
            g_isPlayer = reinterpret_cast<ActorIsPlayerFn>(a);
            logLine("WireOutline: ActorIsPlayer @%p", reinterpret_cast<void*>(a));
        }
    }
    if (!g_hitGetEntity) {
        auto a = bactro::memory::resolve(bactro::memory::SignatureId::HitResultGetEntity);
        if (a) {
            g_hitGetEntity = reinterpret_cast<HitResultGetEntityFn>(a);
            logLine("WireOutline: HitResultGetEntity @%p", reinterpret_cast<void*>(a));
        }
    }
    if (!g_levelGetHitResult) {
        g_levelGetHitResult = bactro::memory::resolve(bactro::memory::SignatureId::LevelGetHitResult);
        if (g_levelGetHitResult)
            logLine("WireOutline: LevelGetHitResult @%p", reinterpret_cast<void*>(g_levelGetHitResult));
    }
    if (!g_fetchNearby) {
        g_fetchNearby = bactro::memory::resolve(bactro::memory::SignatureId::ActorFetchNearbyActorsSorted);
        if (g_fetchNearby)
            logLine("WireOutline: FetchNearby @%p", reinterpret_cast<void*>(g_fetchNearby));
    }
    if (!g_clientGetLocalPlayer) {
        g_clientGetLocalPlayer =
            bactro::memory::resolve(bactro::memory::SignatureId::ClientInstanceGetLocalPlayer);
        if (g_clientGetLocalPlayer)
            logLine("WireOutline: GetLocalPlayer @%p", reinterpret_cast<void*>(g_clientGetLocalPlayer));
    }

    if (!g_tickHooked) {
        void* o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::NormalTick,
                                 reinterpret_cast<void*>(&normalTickDetour), &o) &&
            o) {
            g_tickOriginal = reinterpret_cast<NormalTickFn>(o);
            g_tickHooked = true;
            logLine("WireOutline: NormalTick hooked (local player capture)");
        } else
            logLine("WireOutline: NormalTick FAIL");
    }

    if (!g_fpHooked) {
        void* o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ItemInHandRendererRenderFirstPerson,
                                 reinterpret_cast<void*>(&renderFirstPersonDetour), &o) &&
            o) {
            g_renderFpOriginal = reinterpret_cast<RenderFirstPersonFn>(o);
            g_fpHooked = true;
            logLine("WireOutline: renderFirstPerson hooked (hand/items path)");
        } else
            logLine("WireOutline: renderFirstPerson FAIL");
    }

    logLine("WireOutline: targets hand=%d armor=%d players=%d crystals=%d", g_handItems.load() ? 1 : 0,
            g_armor.load() ? 1 : 0, g_players.load() ? 1 : 0, g_crystals.load() ? 1 : 0);
    logLine("WireOutline: hooks tick=%d fp=%d | next: camera matrix + nearby actors", g_tickHooked ? 1 : 0,
            g_fpHooked ? 1 : 0);
}

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    logLine("WireOutline %s", enabled ? "ON" : "OFF");
    if (enabled) resolveAndHook();
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "handItems")
            g_handItems.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "armor")
            g_armor.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "players")
            g_players.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "crystals")
            g_crystals.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "draw3d")
            g_draw3d.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "lineWidth")
            g_lineWidth.store(std::stof(std::string(value)), std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Wire Outline");
    b.description(
         "White wire on hand/items/weapons, armor, players, crystals.\n"
         "1.26.51: local player capture + GLES ready.\n"
         "Camera matrix still required before lines appear (no fake 2D box).")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("handItems", "Hand / items / weapons", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("armor", "Armor", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("players", "Players", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("crystals", "Crystals", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("draw3d", "3D draw when ready", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("lineWidth", "Line width", pl::modmenu::ConfigType::SliderFloat, "2.0", "1", "5", "");
    b.registerModule();
}

void onSignaturesReady() { logLine("WireOutline: ready — enable after join"); }

void shutdown() {
    g_enabled.store(false, std::memory_order_release);
    g_localPlayer = nullptr;
}

void onPostFrame() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    collectBoxes();
    std::vector<Box> copy;
    {
        std::lock_guard<std::mutex> lock(g_boxMu);
        copy = g_boxes;
    }
    projectAndDraw(copy);
}

} // namespace bactro::wireoutline
