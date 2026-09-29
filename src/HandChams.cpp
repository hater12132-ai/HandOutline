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
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define HC_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::handchams {
namespace {

struct Color {
    float r, g, b, a;
};

constexpr const char* kModuleId = "bactro.handchams";

std::atomic_bool g_enabled{false};
std::atomic_bool g_handOnly{false};
std::atomic_bool g_boxEsp{true};
std::atomic<float> g_r{1.00f};
std::atomic<float> g_g{1.00f};
std::atomic<float> g_b{1.00f};
std::atomic<float> g_opacity{0.70f};
std::atomic_int g_hits{0};

Color g_chams{1.f, 1.f, 1.f, 0.70f};
Color g_outline{1.f, 1.f, 1.f, 1.f};

bool g_renderFpHooked = false;
std::atomic_bool g_entityRenderArmed{false};

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
    float aa = g_opacity.load(std::memory_order_relaxed);
    if (aa < 0.05f) aa = 0.05f;
    if (aa > 1.0f) aa = 1.0f;
    g_chams = {g_r.load(std::memory_order_relaxed), g_g.load(std::memory_order_relaxed),
               g_b.load(std::memory_order_relaxed), aa};
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

void* glProc(const char* name) {
    if (void* p = reinterpret_cast<void*>(eglGetProcAddress(name))) return p;
    static void* h = dlopen("libGLESv2.so", RTLD_NOW);
    if (!h) h = dlopen("libGLESv3.so", RTLD_NOW);
    return h ? dlsym(h, name) : nullptr;
}

struct Box {
    float minx, miny, minz, maxx, maxy, maxz;
};
std::mutex g_boxMu;
std::vector<Box> g_boxes;
std::mutex g_vpMu;
float g_viewProj[16]{};
std::atomic_bool g_vpValid{false};
std::atomic<float> g_lineWidth{2.5f};
std::atomic_int g_modelHits{0};

bool finite16(const float* m) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    return true;
}

bool looksLikeViewProj(const float* m) {
    if (!m || !finite16(m)) return false;
    bool id = true;
    for (int i = 0; i < 16; ++i) {
        float e = (i % 5 == 0) ? 1.f : 0.f;
        if (std::fabs(m[i] - e) > 1e-4f) {
            id = false;
            break;
        }
    }
    if (id) return false;
    float s = 0.f;
    for (int i = 0; i < 16; ++i) s += std::fabs(m[i]);
    if (s < 2.f || s > 1e5f) return false;
    return std::fabs(m[11]) > 0.05f || std::fabs(m[14]) > 0.05f;
}

bool looksLikeModelMatrix(const float* m, float& tx, float& ty, float& tz) {
    if (!m || !finite16(m)) return false;
    if (std::fabs(m[15] - 1.f) > 0.15f) return false;
    tx = m[12];
    ty = m[13];
    tz = m[14];
    if (!std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(tz)) return false;
    // Reject bone/local matrices like (0,12,0) / inventory — need real world XZ
    float xz = std::sqrt(tx * tx + tz * tz);
    if (xz < 16.f) return false;
    if (std::fabs(tx) > 300000.f || std::fabs(tz) > 300000.f) return false;
    if (ty < -80.f || ty > 500.f) return false;
    float sx = std::fabs(m[0]) + std::fabs(m[1]) + std::fabs(m[2]);
    float sy = std::fabs(m[4]) + std::fabs(m[5]) + std::fabs(m[6]);
    float sz = std::fabs(m[8]) + std::fabs(m[9]) + std::fabs(m[10]);
    if (sx < 0.2f || sy < 0.2f || sz < 0.2f) return false;
    if (sx > 20.f || sy > 20.f || sz > 20.f) return false;
    return true;
}

// Actor memory: find a world position (not AABB pair — those were degenerate)
bool probeActorWorldPos(void* actor, float& x, float& y, float& z) {
    if (!actor) return false;
    auto* base = reinterpret_cast<unsigned char*>(actor);
    static const int kOff[] = {
        0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90, 0x98, 0xA0,
        0xA8, 0xB0, 0xB8, 0xC0, 0xC8, 0xD0, 0xE0, 0xF0, 0x100, 0x110, 0x120, 0x128, 0x130, 0x140, 0x148,
        0x150, 0x160, 0x168, 0x180, 0x190, 0x1A0, 0x1C0, 0x1E0, 0x200, 0x220, 0x240, 0x260, 0x280, 0x2A0,
        0x2C0, 0x300, 0x340, 0x380, 0x3C0, 0x400, 0x440, 0x480, 0x4C0, 0x500, 0x540, 0x580
    };
    for (int off : kOff) {
        float* f = reinterpret_cast<float*>(base + off);
        float px = f[0], py = f[1], pz = f[2];
        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) continue;
        float xz = std::sqrt(px * px + pz * pz);
        // world-like: away from origin XZ, reasonable Y
        if (xz < 16.f) continue;
        if (std::fabs(px) > 300000.f || std::fabs(pz) > 300000.f) continue;
        if (py < -64.f || py > 400.f) continue;
        x = px;
        y = py;
        z = pz;
        return true;
    }
    return false;
}

void noteWorldBox(float x, float y, float z) {
    Box b{x - 0.4f, y - 0.1f, z - 0.4f, x + 0.4f, y + 1.8f, z + 0.4f};
    std::lock_guard<std::mutex> lock(g_boxMu);
    if (g_boxes.size() < 128) g_boxes.push_back(b);
}

using PFN_glUniformMatrix4fv = void (*)(int, int, unsigned char, const float*);
PFN_glUniformMatrix4fv g_glUniformMatrix4fvOrig = nullptr;
bool g_glUniformHooked = false;

void glUniformMatrix4fvDetour(int location, int count, unsigned char transpose, const float* value) {
    if (value && count >= 1) {
        float m[16];
        if (transpose) {
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) m[c * 4 + r] = value[r * 4 + c];
        } else {
            std::memcpy(m, value, sizeof(m));
        }
        if (looksLikeViewProj(m)) {
            std::lock_guard<std::mutex> lock(g_vpMu);
            std::memcpy(g_viewProj, m, sizeof(m));
            g_vpValid.store(true, std::memory_order_release);
        } else if (g_enabled.load(std::memory_order_relaxed) && g_boxEsp.load(std::memory_order_relaxed) &&
                   g_entityRenderArmed.load(std::memory_order_acquire)) {
            float tx, ty, tz;
            if (looksLikeModelMatrix(m, tx, ty, tz)) {
                noteWorldBox(tx, ty, tz);
                int n = g_modelHits.fetch_add(1, std::memory_order_relaxed);
                if (n < 8) logLine("SnowChams: model pos (%.1f, %.1f, %.1f)", tx, ty, tz);
            }
        }
    }
    if (g_glUniformMatrix4fvOrig) g_glUniformMatrix4fvOrig(location, count, transpose, value);
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
        logLine("SnowChams: VP+model matrix HOOKED");
        return true;
    }
    logLine("SnowChams: matrix hook FAIL");
    return false;
}

bool worldToNdc(const float* vp, float x, float y, float z, float& ox, float& oy) {
    float clipX = vp[0] * x + vp[4] * y + vp[8] * z + vp[12];
    float clipY = vp[1] * x + vp[5] * y + vp[9] * z + vp[13];
    float clipW = vp[3] * x + vp[7] * y + vp[11] * z + vp[15];
    if (std::fabs(clipW) < 1e-4f) {
        clipX = vp[0] * x + vp[1] * y + vp[2] * z + vp[3];
        clipY = vp[4] * x + vp[5] * y + vp[6] * z + vp[7];
        clipW = vp[12] * x + vp[13] * y + vp[14] * z + vp[15];
    }
    if (std::fabs(clipW) < 1e-4f) return false;
    ox = clipX / clipW;
    oy = clipY / clipW;
    return std::isfinite(ox) && std::isfinite(oy) && ox > -1.2f && ox < 1.2f && oy > -1.2f && oy < 1.2f;
}

using GLuint = unsigned int;
using GLint = int;
using GLenum = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLchar = char;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_LINES = 0x0001;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;

using PFN_glCreateShader = GLuint (*)(GLenum);
using PFN_glShaderSource = void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glCompileShader = void (*)(GLuint);
using PFN_glCreateProgram = GLuint (*)(void);
using PFN_glAttachShader = void (*)(GLuint, GLuint);
using PFN_glLinkProgram = void (*)(GLuint);
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
using PFN_glEnable = void (*)(GLenum);
using PFN_glDisable = void (*)(GLenum);

PFN_glCreateShader d_glCreateShader = nullptr;
PFN_glShaderSource d_glShaderSource = nullptr;
PFN_glCompileShader d_glCompileShader = nullptr;
PFN_glCreateProgram d_glCreateProgram = nullptr;
PFN_glAttachShader d_glAttachShader = nullptr;
PFN_glLinkProgram d_glLinkProgram = nullptr;
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
PFN_glEnable d_glEnable = nullptr;
PFN_glDisable d_glDisable = nullptr;

GLuint g_prog = 0, g_vbo = 0;
GLint g_aPos = -1, g_uColor = -1;
bool g_lineGlReady = false;

bool loadLineGl() {
    if (g_lineGlReady) return true;
#define L(n) d_##n = reinterpret_cast<decltype(d_##n)>(glProc(#n))
    L(glCreateShader); L(glShaderSource); L(glCompileShader); L(glCreateProgram); L(glAttachShader);
    L(glLinkProgram); L(glUseProgram); L(glGetAttribLocation); L(glGetUniformLocation); L(glUniform4f);
    L(glGenBuffers); L(glBindBuffer); L(glBufferData); L(glEnableVertexAttribArray); L(glVertexAttribPointer);
    L(glDrawArrays); L(glBlendFunc); L(glLineWidth); L(glEnable); L(glDisable);
#undef L
    if (!d_glCreateShader || !d_glDrawArrays) return false;
    const char* vs = "attribute vec2 aPos; void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }";
    const char* fs = "precision mediump float; uniform vec4 uColor; void main(){ gl_FragColor = uColor; }";
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
    for (const auto& b : boxes) {
        if ((b.maxx - b.minx) < 0.2f || (b.maxz - b.minz) < 0.2f) continue;
        float c[8][3] = {
            {b.minx, b.miny, b.minz}, {b.maxx, b.miny, b.minz}, {b.maxx, b.maxy, b.minz}, {b.minx, b.maxy, b.minz},
            {b.minx, b.miny, b.maxz}, {b.maxx, b.miny, b.maxz}, {b.maxx, b.maxy, b.maxz}, {b.minx, b.maxy, b.maxz},
        };
        float s[8][2];
        bool ok[8];
        int okn = 0;
        for (int i = 0; i < 8; ++i) {
            ok[i] = worldToNdc(vp, c[i][0], c[i][1], c[i][2], s[i][0], s[i][1]);
            if (ok[i]) ++okn;
        }
        if (okn < 2) continue;
        const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                  {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (auto& e : edges) {
            if (ok[e[0]] && ok[e[1]]) {
                lines.push_back(s[e[0]][0]);
                lines.push_back(s[e[0]][1]);
                lines.push_back(s[e[1]][0]);
                lines.push_back(s[e[1]][1]);
            }
        }
    }
    if (lines.empty()) return;
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (size_t i = 0; i + 1 < lines.size(); i += 2) {
        minx = std::min(minx, lines[i]);
        maxx = std::max(maxx, lines[i]);
        miny = std::min(miny, lines[i + 1]);
        maxy = std::max(maxy, lines[i + 1]);
    }
    if ((maxx - minx) < 0.02f && (maxy - miny) < 0.02f) return;
    if (!loadLineGl()) return;
    if (d_glDisable) d_glDisable(GL_DEPTH_TEST);
    if (d_glEnable) d_glEnable(GL_BLEND);
    if (d_glBlendFunc) d_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (d_glLineWidth) d_glLineWidth(g_lineWidth.load());
    d_glUseProgram(g_prog);
    if (d_glUniform4f && g_uColor >= 0) d_glUniform4f(g_uColor, 1.f, 1.f, 1.f, 0.95f);
    d_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    d_glBufferData(GL_ARRAY_BUFFER, (long)(lines.size() * sizeof(float)), lines.data(), 0x88E4);
    d_glEnableVertexAttribArray((GLuint)g_aPos);
    d_glVertexAttribPointer((GLuint)g_aPos, 2, GL_FLOAT, 0, 0, nullptr);
    d_glDrawArrays(GL_LINES, 0, (GLsizei)(lines.size() / 2));
    d_glUseProgram(0);
    static int s_log = 0;
    if (s_log < 10) {
        logLine("SnowChams: box ESP drew %d segs (%d boxes)", (int)(lines.size() / 4), (int)boxes.size());
        ++s_log;
    }
}

using RenderFirstPersonFn = void (*)(void*, void*, void*, void*, void*, void*);
RenderFirstPersonFn g_renderFpOriginal = nullptr;

void renderFirstPersonDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!g_renderFpOriginal) return;
    bactro::phase::inFirstPersonHand.store(true, std::memory_order_release);
    g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    bactro::phase::inFirstPersonHand.store(false, std::memory_order_release);
}

using SetEntityConstantsFn = void (*)(void*, void*, const Color*, const void*, const void*, const Color*,
                                      const Color*, const Color*, const Color*, const void*, const void*, float,
                                      float, float, float);
SetEntityConstantsFn g_setEntityConstants = nullptr;
bool g_hookedEntity = false;

void setEntityConstantsDetour(void* entityConstants, void* renderContext, const Color* tileLightColor,
                              const void* tileLightColorUV, const void* blockLightColor, const Color* overlay,
                              const Color* changeColor, const Color* changeColor2, const Color* glintColor,
                              const void* glintUVScale, const void* uvAnim, float uvOffset1, float uvOffset2,
                              float uvRot1, float uvRot2) {
    if (!g_setEntityConstants) return;
    // Hand path only for setEntityConstants (avoids inventory paper-doll wash)
    const bool fp = bactro::phase::inFirstPersonHand.load(std::memory_order_acquire);
    if (!g_enabled.load(std::memory_order_relaxed) || (!fp && g_handOnly.load(std::memory_order_relaxed))) {
        g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                             overlay, changeColor, changeColor2, glintColor, glintUVScale, uvAnim, uvOffset1,
                             uvOffset2, uvRot1, uvRot2);
        return;
    }
    if (!fp && !shouldApply()) {
        g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                             overlay, changeColor, changeColor2, glintColor, glintUVScale, uvAnim, uvOffset1,
                             uvOffset2, uvRot1, uvRot2);
        return;
    }
    // Prefer FP hand for this hook; entity bodies use setupActorGlint(worldActor)
    if (!fp) {
        g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                             overlay, changeColor, changeColor2, glintColor, glintUVScale, uvAnim, uvOffset1,
                             uvOffset2, uvRot1, uvRot2);
        return;
    }
    refresh();
    g_entityRenderArmed.store(true, std::memory_order_release);
    g_setEntityConstants(entityConstants, renderContext, &g_chams, tileLightColorUV, blockLightColor, &g_chams,
                         &g_chams, &g_chams, &g_outline, glintUVScale, uvAnim, uvOffset1, uvOffset2, uvRot1, uvRot2);
}

using SetupActorGlintFn = void (*)(void*, void*, void*, const Color*, const Color*, const Color*, const Color*,
                                   float, float, float, float, const void*);
SetupActorGlintFn g_setupActorGlint = nullptr;
bool g_hookedActor = false;

void setupActorGlintDetour(void* screenContext, void* entityContext, void* actor, const Color* overlay,
                           const Color* changeColor, const Color* changeColor2, const Color* glintColor,
                           float uvOffset1, float uvOffset2, float uvRot1, float uvRot2,
                           const void* lightEmissionColor) {
    if (!g_setupActorGlint) return;

    float ax = 0, ay = 0, az = 0;
    const bool worldActor = actor && probeActorWorldPos(actor, ax, ay, az);
    if (worldActor && g_enabled.load(std::memory_order_relaxed) && g_boxEsp.load(std::memory_order_relaxed)) {
        noteWorldBox(ax, ay, az);
        static int s_log = 0;
        if (s_log < 8) {
            logLine("SnowChams: actor world (%.1f, %.1f, %.1f)", ax, ay, az);
            ++s_log;
        }
    }

    // Inventory paper-doll / local actors: never recolor
    if (!shouldApply() || (actor && !worldActor)) {
        g_setupActorGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor,
                          uvOffset1, uvOffset2, uvRot1, uvRot2, lightEmissionColor);
        return;
    }
    refresh();
    g_entityRenderArmed.store(true, std::memory_order_release);
    g_setupActorGlint(screenContext, entityContext, actor, &g_chams, &g_chams, &g_chams, &g_outline, uvOffset1,
                      uvOffset2, uvRot1, uvRot2, lightEmissionColor);
}

SetupActorGlintFn g_setupGlint = nullptr;
bool g_hookedGlint = false;

void setupGlintDetour(void* screenContext, void* entityContext, void* actor, const Color* overlay,
                      const Color* changeColor, const Color* changeColor2, const Color* glintColor, float uvOffset1,
                      float uvOffset2, float uvRot1, float uvRot2, const void* lightEmissionColor) {
    if (!g_setupGlint) return;
    if (!shouldApply()) {
        g_setupGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor, uvOffset1,
                     uvOffset2, uvRot1, uvRot2, lightEmissionColor);
        return;
    }
    refresh();
    g_entityRenderArmed.store(true, std::memory_order_release);
    g_setupGlint(screenContext, entityContext, actor, &g_chams, &g_chams, &g_chams, &g_outline, uvOffset1, uvOffset2,
                 uvRot1, uvRot2, lightEmissionColor);
}

void tryInstallHooks() {
    void* o = nullptr;
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
    logLine("SnowChams: hooks fp=%d entity=%d actor=%d glint=%d matrix=%d", g_renderFpHooked ? 1 : 0,
            g_hookedEntity ? 1 : 0, g_hookedActor ? 1 : 0, g_hookedGlint ? 1 : 0, g_glUniformHooked ? 1 : 0);
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
        else if (key == "handOnly")
            g_handOnly.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "boxEsp")
            g_boxEsp.store(value == "true" || value == "1", std::memory_order_relaxed);
        refresh();
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Snow Chams ESP");
    b.description("White hand chams + world box ESP from model matrices. No intensity slider.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("r", "Red", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("g", "Green", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("b", "Blue", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("opacity", "Opacity", pl::modmenu::ConfigType::SliderFloat, "0.70", "0.05", "1.0", "");
    b.config("boxEsp", "White box ESP", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("handOnly", "Hand/items only", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.registerModule();
}

void onSignaturesReady() {
    logLine("SnowChams: ready — join world, then enable");
    if (g_enabled.load(std::memory_order_relaxed)) {
        logLine("SnowChams: was ON during resolve — installing hooks now");
        tryInstallHooks();
    }
}

void shutdown() { g_enabled.store(false, std::memory_order_release); }

void onPostFrame() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    drawBoxEsp();
    g_entityRenderArmed.store(false, std::memory_order_release);
}

} // namespace bactro::handchams
