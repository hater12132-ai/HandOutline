#include "bactro/HandChams.hpp"
#include "bactro/RenderPhase.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <EGL/egl.h>
#include <android/log.h>
#include <dlfcn.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
#include <vector>
#include <cmath>

#define HC_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::handchams {
namespace {

struct Color {
    float r, g, b, a;
};

constexpr const char* kModuleId = "bactro.handchams";

std::atomic_bool g_enabled{false};
std::atomic_bool g_handOnly{false};
std::atomic_bool g_outlinePass{false}; // mesh multipass — weak on MC models; use box ESP
std::atomic_bool g_throughWalls{false};
std::atomic<float> g_r{1.00f};
std::atomic<float> g_g{1.00f};
std::atomic<float> g_b{1.00f};
std::atomic<float> g_opacity{0.70f};
std::atomic<float> g_intensity{1.5f};
std::atomic_int g_hits{0};
std::atomic_int g_meshHits{0};

Color g_chams{1.00f, 1.00f, 1.00f, 0.70f};
Color g_outline{1.00f, 1.00f, 1.00f, 1.00f};

bool g_renderFpHooked = false;
std::atomic_bool g_meshChamsArmed{false}; // setEntityConstants saw a chams-eligible draw
std::atomic_int g_pass{0}; // 0=normal, 1=outline, 2=fill

// ---- White AABB ESP (visible outline around entities) ----
struct Box {
    float minx, miny, minz, maxx, maxy, maxz;
};
std::mutex g_boxMu;
std::vector<Box> g_boxes;
std::mutex g_vpMu;
float g_viewProj[16]{};
std::atomic_bool g_vpValid{false};
std::atomic_bool g_boxEsp{true};
std::atomic<float> g_lineWidth{2.5f};

void logLine(const char* fmt, ...);

bool finite3(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}
bool looksLikeWorldPos(float x, float y, float z) {
    if (!finite3(x, y, z)) return false;
    if (std::fabs(x) > 300000.f || std::fabs(z) > 300000.f) return false;
    if (y < -80.f || y > 400.f) return false;
    if (std::fabs(x) < 1e-3f && std::fabs(y) < 1e-3f && std::fabs(z) < 1e-3f) return false;
    return true;
}
bool probeActorBox(void* actor, Box& out) {
    if (!actor) return false;
    auto* base = reinterpret_cast<unsigned char*>(actor);
    // Wider scan — Bedrock actor layout moves a lot by version
    static const int kPosOff[] = {
        0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80,
        0x88, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8, 0xC0, 0xC8, 0xD0, 0xD8, 0xE0,
        0xF0, 0xF8, 0x100, 0x108, 0x110, 0x118, 0x120, 0x128, 0x130, 0x138, 0x140,
        0x148, 0x150, 0x158, 0x160, 0x168, 0x170, 0x180, 0x190, 0x1A0, 0x1B0, 0x1C0,
        0x1D0, 0x1E0, 0x1F0, 0x200, 0x210, 0x220, 0x230, 0x240, 0x250, 0x260, 0x280,
        0x2A0, 0x2C0, 0x2E0, 0x300, 0x320, 0x340, 0x360, 0x380, 0x3A0, 0x3C0, 0x3E0,
        0x400, 0x420, 0x440, 0x460, 0x480, 0x4A0, 0x4C0, 0x500, 0x540, 0x580, 0x5C0
    };
    // Prefer a true AABB pair (min/max floats) if present
    for (int off : kPosOff) {
        float* f = reinterpret_cast<float*>(base + off);
        float x0=f[0], y0=f[1], z0=f[2], x1=f[3], y1=f[4], z1=f[5];
        if (looksLikeWorldPos(x0, y0, z0) && looksLikeWorldPos(x1, y1, z1) &&
            x1 > x0 && y1 > y0 && z1 > z0 && (x1-x0) < 4.f && (y1-y0) < 4.f && (z1-z0) < 4.f &&
            (y1-y0) > 0.5f) {
            out = {x0, y0, z0, x1, y1, z1};
            return true;
        }
    }
    for (int off : kPosOff) {
        float* f = reinterpret_cast<float*>(base + off);
        if (looksLikeWorldPos(f[0], f[1], f[2])) {
            // Skip near-origin (inventory paper-doll / local space junk)
            if (std::fabs(f[0]) < 8.f && std::fabs(f[2]) < 8.f && std::fabs(f[1]) < 8.f)
                continue;
            float x = f[0], y = f[1], z = f[2];
            out = {x - 0.4f, y, z - 0.4f, x + 0.4f, y + 1.9f, z + 0.4f};
            return true;
        }
    }
    return false;
}
void noteActorEsp(void* actor) {
    if (!g_boxEsp.load(std::memory_order_relaxed) || !actor) return;
    Box b{};
    if (!probeActorBox(actor, b)) return;
    // Drop inventory-local boxes (very small world coords near 0)
    float cx = 0.5f * (b.minx + b.maxx);
    float cz = 0.5f * (b.minz + b.maxz);
    if (std::fabs(cx) < 8.f && std::fabs(cz) < 8.f) return;
    static int s_boxLog = 0;
    if (s_boxLog < 6) {
        logLine("SnowChams: actor box (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)", b.minx, b.miny, b.minz, b.maxx, b.maxy,
                b.maxz);
        ++s_boxLog;
    }
    std::lock_guard<std::mutex> lock(g_boxMu);
    if (g_boxes.size() < 96)
        g_boxes.push_back(b);
}


void logLine(const char* fmt, ...) {
    char buf[220];
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
    g_outline = {1.f, 1.f, 1.f, 1.f};
}

bool shouldApply() {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    if (g_handOnly.load(std::memory_order_relaxed)) {
        if (g_renderFpHooked)
            return bactro::phase::inFirstPersonHand.load(std::memory_order_acquire);
        return true;
    }
    return true;
}

// ---- GLES helpers (optional outline pass) ----
using PFN_void1 = void (*)(unsigned int);
using PFN_void1i = void (*)(unsigned char);
using PFN_depth = void (*)(unsigned int);
using PFN_get = void (*)(unsigned int, int*);

PFN_void1 p_glEnable = nullptr;
PFN_void1 p_glDisable = nullptr;
PFN_void1 p_glCullFace = nullptr;
PFN_void1i p_glDepthMask = nullptr;
PFN_depth p_glDepthFunc = nullptr;
PFN_get p_glGetIntegerv = nullptr;
bool g_glResolved = false;

void* glProc(const char* name) {
    if (void* p = reinterpret_cast<void*>(eglGetProcAddress(name))) return p;
    static void* h = dlopen("libGLESv2.so", RTLD_NOW);
    return h ? dlsym(h, name) : nullptr;
}

void resolveGl() {
    if (g_glResolved) return;
    p_glEnable = reinterpret_cast<PFN_void1>(glProc("glEnable"));
    p_glDisable = reinterpret_cast<PFN_void1>(glProc("glDisable"));
    p_glCullFace = reinterpret_cast<PFN_void1>(glProc("glCullFace"));
    p_glDepthMask = reinterpret_cast<PFN_void1i>(glProc("glDepthMask"));
    p_glDepthFunc = reinterpret_cast<PFN_depth>(glProc("glDepthFunc"));
    p_glGetIntegerv = reinterpret_cast<PFN_get>(glProc("glGetIntegerv"));
    g_glResolved = p_glCullFace && p_glDepthMask && p_glEnable;
    if (g_glResolved)
        logLine("SnowChams: GLES outline helpers OK");
    else
        logLine("SnowChams: GLES outline helpers partial");
}

constexpr unsigned int GL_CULL_FACE = 0x0B44;
constexpr unsigned int GL_FRONT = 0x0404;
constexpr unsigned int GL_BACK = 0x0405;
constexpr unsigned int GL_DEPTH_TEST = 0x0B71;
constexpr unsigned int GL_ALWAYS = 0x0207;
constexpr unsigned int GL_LEQUAL = 0x0203;
constexpr unsigned int GL_LESS = 0x0201;

void beginOutlineGl() {
    if (!g_glResolved) return;
    if (p_glEnable) p_glEnable(GL_CULL_FACE);
    if (p_glCullFace) p_glCullFace(GL_FRONT); // inverted hull silhouette
    if (p_glDepthMask) p_glDepthMask(0);
    if (g_throughWalls.load(std::memory_order_relaxed) && p_glDepthFunc)
        p_glDepthFunc(GL_ALWAYS);
}

void endOutlineGl() {
    if (!g_glResolved) return;
    if (p_glCullFace) p_glCullFace(GL_BACK);
    if (p_glDepthMask) p_glDepthMask(1);
    if (p_glDepthFunc) p_glDepthFunc(GL_LEQUAL);
}

// ---- FP hand phase ----
using RenderFirstPersonFn = void (*)(void*, void*, void*, void*, void*, void*);
RenderFirstPersonFn g_renderFpOriginal = nullptr;

void renderFirstPersonDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!g_renderFpOriginal) return;
    bactro::phase::inFirstPersonHand.store(true, std::memory_order_release);
    g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    bactro::phase::inFirstPersonHand.store(false, std::memory_order_release);
}

// ---- setEntityConstants: arm chams + push fill or outline colors ----
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
    g_meshChamsArmed.store(true, std::memory_order_release);

    const int pass = g_pass.load(std::memory_order_relaxed);
    const Color* fill = (pass == 1) ? &g_outline : &g_chams;
    const Color* glint = (pass == 1 || g_outlinePass.load(std::memory_order_relaxed)) ? &g_outline : glintColor;

    g_setEntityConstants(entityConstants, renderContext, fill, tileLightColorUV, blockLightColor, fill, fill, fill,
                         glint, glintUVScale, uvAnim, uvOffset1, uvOffset2, uvRot1, uvRot2);

    const int n = g_hits.fetch_add(1, std::memory_order_relaxed);
    if (n < 10)
        logLine("SnowChams: constants #%d pass=%d", n, pass);
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
    if (g_enabled.load(std::memory_order_relaxed))
        noteActorEsp(actor);
    if (!shouldApply()) {
        g_setupActorGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor,
                          uvOffset1, uvOffset2, uvRot1, uvRot2, lightEmissionColor);
        return;
    }
    refresh();
    g_meshChamsArmed.store(true, std::memory_order_release);
    const int pass = g_pass.load(std::memory_order_relaxed);
    const Color* fill = (pass == 1) ? &g_outline : &g_chams;
    const Color* glint = &g_outline;
    g_setupActorGlint(screenContext, entityContext, actor, fill, fill, fill, glint, uvOffset1, uvOffset2, uvRot1,
                      uvRot2, lightEmissionColor);
}

SetupActorGlintFn g_setupGlint = nullptr;
bool g_hookedGlint = false;

void setupGlintDetour(
    void* screenContext, void* entityContext, void* actor, const Color* overlay, const Color* changeColor,
    const Color* changeColor2, const Color* glintColor, float uvOffset1, float uvOffset2, float uvRot1,
    float uvRot2, const void* lightEmissionColor) {
    if (!g_setupGlint) return;
    if (!shouldApply()) {
        g_setupGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor, uvOffset1,
                     uvOffset2, uvRot1, uvRot2, lightEmissionColor);
        return;
    }
    refresh();
    g_meshChamsArmed.store(true, std::memory_order_release);
    const Color* fill = (g_pass.load(std::memory_order_relaxed) == 1) ? &g_outline : &g_chams;
    g_setupGlint(screenContext, entityContext, actor, fill, fill, fill, &g_outline, uvOffset1, uvOffset2, uvRot1,
                 uvRot2, lightEmissionColor);
}

// ---- Multi-pass mesh draw (inverted-cull outline + fill) ----
// ARM64: keep a wide arg list so we don't clobber X2–X7 (sig uses at least X0–X4).
using RenderMeshFn = void (*)(void*, void*, void*, void*, void*, void*, void*, void*);
RenderMeshFn g_renderMeshOrig = nullptr;
bool g_hookedMesh = false;

void renderMeshDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    if (!g_renderMeshOrig) return;

    const bool armed = g_meshChamsArmed.load(std::memory_order_acquire);
    const bool doOutline = g_enabled.load(std::memory_order_relaxed) && armed &&
                           g_outlinePass.load(std::memory_order_relaxed);

    if (!doOutline) {
        g_renderMeshOrig(a0, a1, a2, a3, a4, a5, a6, a7);
        if (armed) g_meshChamsArmed.store(false, std::memory_order_release);
        return;
    }

    resolveGl();

    // Pass 1: white silhouette (front-face cull → backfaces = outline on blocky meshes)
    g_pass.store(1, std::memory_order_relaxed);
    beginOutlineGl();
    g_renderMeshOrig(a0, a1, a2, a3, a4, a5, a6, a7);
    endOutlineGl();

    // Pass 2: snow fill
    g_pass.store(2, std::memory_order_relaxed);
    g_renderMeshOrig(a0, a1, a2, a3, a4, a5, a6, a7);

    g_pass.store(0, std::memory_order_relaxed);
    g_meshChamsArmed.store(false, std::memory_order_release);

    const int n = g_meshHits.fetch_add(1, std::memory_order_relaxed);
    if (n < 12)
        logLine("SnowChams: multipass mesh #%d", n);
}


// ---- View-projection capture for box ESP ----
using PFN_glUniformMatrix4fv = void (*)(int, int, unsigned char, const float*);
PFN_glUniformMatrix4fv g_glUniformMatrix4fvOrig = nullptr;
bool g_glUniformHooked = false;

bool looksLikeViewProj(const float* m) {
    if (!m) return false;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    // Reject identity
    bool id = true;
    for (int i = 0; i < 16; ++i) {
        float expect = (i % 5 == 0) ? 1.f : 0.f;
        if (std::fabs(m[i] - expect) > 1e-4f) { id = false; break; }
    }
    if (id) return false;
    // Prefer real camera matrices: non-zero perspective row, finite w scale
    float sumAbs = 0.f;
    for (int i = 0; i < 16; ++i) sumAbs += std::fabs(m[i]);
    if (sumAbs < 1.f || sumAbs > 1e6f) return false;
    // m[11] often -1 for perspective (column-major viewproj)
    if (std::fabs(m[11]) < 0.01f && std::fabs(m[14]) < 0.01f) return false;
    return true;
}

void glUniformMatrix4fvDetour(int location, int count, unsigned char transpose, const float* value) {
    if (value && count >= 1 && looksLikeViewProj(value)) {
        std::lock_guard<std::mutex> lock(g_vpMu);
        if (transpose) {
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    g_viewProj[c * 4 + r] = value[r * 4 + c];
        } else {
            std::memcpy(g_viewProj, value, 16 * sizeof(float));
        }
        g_vpValid.store(true, std::memory_order_release);
    }
    if (g_glUniformMatrix4fvOrig)
        g_glUniformMatrix4fvOrig(location, count, transpose, value);
}

bool installVpHook() {
    if (g_glUniformHooked) return true;
    void* target = glProc("glUniformMatrix4fv");
    if (!target) {
        logLine("SnowChams: glUniformMatrix4fv not found");
        return false;
    }
    void* o = nullptr;
    if (pl::memory::hook(target, reinterpret_cast<void*>(&glUniformMatrix4fvDetour), &o) == 0 && o) {
        g_glUniformMatrix4fvOrig = reinterpret_cast<PFN_glUniformMatrix4fv>(o);
        g_glUniformHooked = true;
        logLine("SnowChams: VP capture HOOKED");
        return true;
    }
    logLine("SnowChams: VP capture FAIL");
    return false;
}

bool worldToNdc(const float* vp, float x, float y, float z, float& ox, float& oy) {
    float clipX = vp[0]*x + vp[4]*y + vp[8]*z + vp[12];
    float clipY = vp[1]*x + vp[5]*y + vp[9]*z + vp[13];
    float clipW = vp[3]*x + vp[7]*y + vp[11]*z + vp[15];
    if (std::fabs(clipW) < 1e-5f) return false;
    ox = clipX / clipW;
    oy = clipY / clipW;
    return std::isfinite(ox) && std::isfinite(oy) && ox > -2.f && ox < 2.f && oy > -2.f && oy < 2.f;
}

// Minimal line shader for NDC boxes
using GLuint = unsigned int;
using GLint = int;
using GLenum = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLchar = char;
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

using PFN_glCreateShader = GLuint (*)(GLenum);
using PFN_glShaderSource = void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glCompileShader = void (*)(GLuint);
using PFN_glGetShaderiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glCreateProgram = GLuint (*)(void);
using PFN_glAttachShader = void (*)(GLuint, GLuint);
using PFN_glLinkProgram = void (*)(GLuint);
using PFN_glGetProgramiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glUseProgram = void (*)(GLuint);
using PFN_glGetAttribLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glGetUniformLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glUniform4f = void (*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glGenBuffers = void (*)(GLsizei, GLuint*);
using PFN_glBindBuffer = void (*)(GLenum, GLuint);
using PFN_glBufferData = void (*)(GLenum, long, const void*, GLenum);
using PFN_glEnableVertexAttribArray = void (*)(GLuint);
using PFN_glVertexAttribPointer = void (*)(GLuint, GLint, GLenum, unsigned char, GLsizei, const void*);
using PFN_glDrawArrays = void (*)(GLenum, GLint, GLsizei);
using PFN_glBlendFunc = void (*)(GLenum, GLenum);
using PFN_glLineWidth = void (*)(GLfloat);

PFN_glCreateShader d_glCreateShader = nullptr;
PFN_glShaderSource d_glShaderSource = nullptr;
PFN_glCompileShader d_glCompileShader = nullptr;
PFN_glGetShaderiv d_glGetShaderiv = nullptr;
PFN_glCreateProgram d_glCreateProgram = nullptr;
PFN_glAttachShader d_glAttachShader = nullptr;
PFN_glLinkProgram d_glLinkProgram = nullptr;
PFN_glGetProgramiv d_glGetProgramiv = nullptr;
PFN_glUseProgram d_glUseProgram = nullptr;
PFN_glGetAttribLocation d_glGetAttribLocation = nullptr;
PFN_glGetUniformLocation d_glGetUniformLocation = nullptr;
PFN_glUniform4f d_glUniform4f = nullptr;
PFN_glGenBuffers d_glGenBuffers = nullptr;
PFN_glBindBuffer d_glBindBuffer = nullptr;
PFN_glBufferData d_glBufferData = nullptr;
PFN_glEnableVertexAttribArray d_glEnableVertexAttribArray = nullptr;
PFN_glVertexAttribPointer d_glVertexAttribPointer = nullptr;
PFN_glDrawArrays d_glDrawArrays = nullptr;
PFN_glBlendFunc d_glBlendFunc = nullptr;
PFN_glLineWidth d_glLineWidth = nullptr;

GLuint g_prog = 0, g_vbo = 0;
GLint g_aPos = -1, g_uColor = -1;
bool g_lineGlReady = false;

bool loadLineGl() {
    if (g_lineGlReady) return true;
#define L(name) d_##name = reinterpret_cast<decltype(d_##name)>(glProc(#name))
    L(glCreateShader); L(glShaderSource); L(glCompileShader); L(glGetShaderiv);
    L(glCreateProgram); L(glAttachShader); L(glLinkProgram); L(glGetProgramiv);
    L(glUseProgram); L(glGetAttribLocation); L(glGetUniformLocation); L(glUniform4f);
    L(glGenBuffers); L(glBindBuffer); L(glBufferData);
    L(glEnableVertexAttribArray); L(glVertexAttribPointer); L(glDrawArrays);
    L(glBlendFunc); L(glLineWidth);
#undef L
    if (!d_glCreateShader || !d_glCreateProgram || !d_glDrawArrays) return false;
    const char* vs =
        "attribute vec2 aPos; void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }";
    const char* fs =
        "precision mediump float; uniform vec4 uColor; void main(){ gl_FragColor = uColor; }";
    GLuint v = d_glCreateShader(GL_VERTEX_SHADER);
    GLuint f = d_glCreateShader(GL_FRAGMENT_SHADER);
    d_glShaderSource(v, 1, &vs, nullptr);
    d_glCompileShader(v);
    d_glShaderSource(f, 1, &fs, nullptr);
    d_glCompileShader(f);
    g_prog = d_glCreateProgram();
    d_glAttachShader(g_prog, v);
    d_glAttachShader(g_prog, f);
    d_glLinkProgram(g_prog);
    g_aPos = d_glGetAttribLocation(g_prog, "aPos");
    g_uColor = d_glGetUniformLocation(g_prog, "uColor");
    d_glGenBuffers(1, &g_vbo);
    g_lineGlReady = g_prog != 0;
    if (g_lineGlReady) logLine("SnowChams: box ESP line shader OK");
    return g_lineGlReady;
}

void drawBoxEsp() {
    if (!g_boxEsp.load(std::memory_order_relaxed)) return;
    if (!g_vpValid.load(std::memory_order_acquire)) return;
    std::vector<Box> boxes;
    {
        std::lock_guard<std::mutex> lock(g_boxMu);
        boxes.swap(g_boxes);
    }
    if (boxes.empty()) return;
    float vp[16];
    {
        std::lock_guard<std::mutex> lock(g_vpMu);
        std::memcpy(vp, g_viewProj, sizeof(vp));
    }
    std::vector<float> lines;
    lines.reserve(boxes.size() * 48);
    for (const auto& b : boxes) {
        float c[8][3] = {
            {b.minx, b.miny, b.minz}, {b.maxx, b.miny, b.minz}, {b.maxx, b.maxy, b.minz}, {b.minx, b.maxy, b.minz},
            {b.minx, b.miny, b.maxz}, {b.maxx, b.miny, b.maxz}, {b.maxx, b.maxy, b.maxz}, {b.minx, b.maxy, b.maxz},
        };
        float s[8][2];
        bool ok[8];
        for (int i = 0; i < 8; ++i)
            ok[i] = worldToNdc(vp, c[i][0], c[i][1], c[i][2], s[i][0], s[i][1]);
        const int edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
        for (auto& e : edges) {
            if (ok[e[0]] && ok[e[1]]) {
                lines.push_back(s[e[0]][0]); lines.push_back(s[e[0]][1]);
                lines.push_back(s[e[1]][0]); lines.push_back(s[e[1]][1]);
            }
        }
    }
    if (lines.empty()) return;
    if (!loadLineGl()) return;
    if (p_glDisable) p_glDisable(GL_DEPTH_TEST);
    if (p_glEnable) p_glEnable(GL_BLEND);
    if (d_glBlendFunc) d_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (d_glLineWidth) d_glLineWidth(g_lineWidth.load());
    d_glUseProgram(g_prog);
    if (d_glUniform4f && g_uColor >= 0)
        d_glUniform4f(g_uColor, 1.f, 1.f, 1.f, 0.95f);
    d_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    d_glBufferData(GL_ARRAY_BUFFER, (long)(lines.size() * sizeof(float)), lines.data(), 0x88E4);
    d_glEnableVertexAttribArray((GLuint)g_aPos);
    d_glVertexAttribPointer((GLuint)g_aPos, 2, GL_FLOAT, 0, 0, nullptr);
    d_glDrawArrays(GL_LINES, 0, (GLsizei)(lines.size() / 2));
    d_glUseProgram(0);
    static int s_log = 0;
    if (s_log < 8) {
        logLine("SnowChams: box ESP drew %d segs (%d actors)", (int)(lines.size()/4), (int)boxes.size());
        ++s_log;
    }
}

void tryInstallHooks() {
    void* o = nullptr;
    resolveGl();
    installVpHook();

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
            g_setupGlint = reinterpret_cast<SetupActorGlintFn>(o);
            g_hookedGlint = true;
            logLine("HandChams: setupGlint hooked");
        } else
            logLine("HandChams: setupGlint FAIL");
    }

    if (!g_hookedMesh) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::MeshHelpersRenderMeshImmediately,
                                 reinterpret_cast<void*>(&renderMeshDetour), &o) &&
            o) {
            g_renderMeshOrig = reinterpret_cast<RenderMeshFn>(o);
            g_hookedMesh = true;
            logLine("SnowChams: RenderMeshImmediately HOOKED (multipass outline)");
        } else {
            // try alternate signature
            o = nullptr;
            if (bactro::memory::hook(bactro::memory::SignatureId::MeshHelpersRenderMeshImmediately2,
                                     reinterpret_cast<void*>(&renderMeshDetour), &o) &&
                o) {
                g_renderMeshOrig = reinterpret_cast<RenderMeshFn>(o);
                g_hookedMesh = true;
                logLine("SnowChams: RenderMeshImmediately2 HOOKED (multipass outline)");
            } else
                logLine("SnowChams: RenderMesh FAIL (outline needs this)");
        }
    }

    logLine("SnowChams: hooks fp=%d entity=%d actor=%d glint=%d mesh=%d", g_renderFpHooked ? 1 : 0,
            g_hookedEntity ? 1 : 0, g_hookedActor ? 1 : 0, g_hookedGlint ? 1 : 0, g_hookedMesh ? 1 : 0);
}

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    logLine("SnowChams %s", enabled ? "ON" : "OFF");
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
        else if (key == "outlinePass")
            g_outlinePass.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "throughWalls")
            g_throughWalls.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "boxEsp")
            g_boxEsp.store(value == "true" || value == "1", std::memory_order_relaxed);
        refresh();
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Snow Chams ESP");
    b.description(
         "Snowy hand/entity chams + white AABB box ESP on rendered actors. "
         "Box ESP is the visible outline (mesh multipass is off by default). Join world, enable.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("r", "Red", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("g", "Green", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("b", "Blue", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("opacity", "Opacity", pl::modmenu::ConfigType::SliderFloat, "0.70", "0.05", "1.0", "");
    b.config("intensity", "Intensity", pl::modmenu::ConfigType::SliderFloat, "1.50", "0.1", "2.5", "");
    b.config("outlinePass", "Mesh multipass (experimental)", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.config("throughWalls", "Outline through walls", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.config("boxEsp", "White box ESP (entities)", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("handOnly", "Hand/items only", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.registerModule();
}

void onSignaturesReady() {
    logLine("SnowChams: ready — join world, then enable");
}

void shutdown() { g_enabled.store(false, std::memory_order_release); }

void onPostFrame() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    drawBoxEsp();
}

} // namespace bactro::handchams
