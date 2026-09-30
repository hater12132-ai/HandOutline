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
std::atomic_bool g_handOnly{false}; // false = allow world actors when targets enabled
std::atomic_bool g_targetPlayers{true};
std::atomic_bool g_targetMobs{true};   // non-player actors
std::atomic_bool g_targetHand{true};   // FP hand / null actor (items, cosmetics-ish)
std::atomic_bool g_skyShader{false};   // Lexora-style plasma/aurora on hand+items
std::atomic<float> g_skySpeed{1.0f};   // animation speed
std::atomic<float> g_skyOpacity{0.85f};
std::atomic<int> g_skyMode{0}; // 0=plasma 1=aurora 2=midnight 3=nebula
std::atomic<int> g_fpSticky{0};
std::atomic_bool g_boxEsp{false};
std::atomic_bool g_playersOnly{true};
std::atomic<float> g_r{1.00f};
std::atomic<float> g_g{1.00f};
std::atomic<float> g_b{1.00f};
std::atomic<float> g_opacity{0.85f};

Color g_chams{1.f, 1.f, 1.f, 0.85f};
Color g_outline{1.f, 1.f, 1.f, 1.f};

bool g_renderFpHooked = false;
bool g_hookedEntity = false;
bool g_hookedActor = false;
bool g_hookedGlint = false;

void logLine(const char* fmt, ...) {
    char buf[220];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    HC_LOGI("%s", buf);
}


// Lexora sky modes → bright animated colors for Color* path (hand/items only)
Color skyChamsColor() {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    const float speed = g_skySpeed.load(std::memory_order_relaxed);
    const float opac = g_skyOpacity.load(std::memory_order_relaxed);
    const int mode = g_skyMode.load(std::memory_order_relaxed);
    const double sec = std::chrono::duration<double>(clock::now() - t0).count();
    const float t = static_cast<float>(sec) * speed;

    float r = 1.f, g = 1.f, b = 1.f;

    if (mode == 1) {
        // aurora: green / cyan / magenta bands
        float w = 0.5f + 0.5f * std::sin(t * 1.4f);
        float w2 = 0.5f + 0.5f * std::sin(t * 0.9f + 1.7f);
        r = 0.25f + 0.55f * w2;
        g = 0.55f + 0.45f * w;
        b = 0.70f + 0.30f * (1.f - w);
    } else if (mode == 2) {
        // midnight: deep blue + moon silver pulse
        float star = 0.5f + 0.5f * std::sin(t * 2.2f);
        r = 0.12f + 0.25f * star;
        g = 0.18f + 0.30f * star;
        b = 0.45f + 0.50f * star;
    } else if (mode == 3) {
        // nebula: purple / blue swirl
        float f = 0.5f + 0.5f * std::sin(t * 1.1f);
        float f2 = 0.5f + 0.5f * std::cos(t * 0.7f);
        r = 0.45f + 0.40f * f;
        g = 0.15f + 0.25f * f2;
        b = 0.70f + 0.30f * (1.f - f);
    } else {
        // plasma (default): violet → pink → cyan (Lexora plasma_sky, boosted)
        float px = 3.f + t * 0.5f, py = 1.5f + t * 0.3f, pz = 2.f + t * 0.4f;
        float flow = 0.f, amp = 1.f;
        for (int i = 0; i < 5; ++i) {
            flow += amp * std::fabs(std::sin(px) * std::cos(py) + std::sin(pz));
            float nx = 0.36f * px + 0.48f * py - 0.80f * pz;
            float ny = -0.80f * px + 0.60f * py;
            float nz = 0.48f * px + 0.64f * py + 0.60f * pz;
            px = nx * 1.3f; py = ny * 1.3f; pz = nz * 1.3f;
            amp *= 0.6f;
        }
        float x = (flow - 0.5f) / 2.f;
        if (x < 0.f) x = 0.f;
        if (x > 1.f) x = 1.f;
        flow = x * x * (3.f - 2.f * x);
        // bright mix so it is not near-black
        float bg[3] = {0.20f, 0.05f, 0.35f};
        float c1[3] = {0.95f, 0.25f, 0.70f};
        float c2[3] = {0.20f, 0.90f, 1.00f};
        float col[3];
        for (int i = 0; i < 3; ++i) col[i] = bg[i] + (c1[i] - bg[i]) * flow;
        float hi = (flow - 0.5f) / 0.5f;
        if (hi < 0.f) hi = 0.f;
        if (hi > 1.f) hi = 1.f;
        hi = hi * hi * (3.f - 2.f * hi);
        for (int i = 0; i < 3; ++i) col[i] = col[i] + (c2[i] - col[i]) * hi;
        r = col[0]; g = col[1]; b = col[2];
    }

    // floor brightness so RD doesn't read as black
    const float floor = 0.15f;
    if (r < floor) r = floor;
    if (g < floor) g = floor;
    if (b < floor) b = floor;
    if (r > 1.f) r = 1.f;
    if (g > 1.f) g = 1.f;
    if (b > 1.f) b = 1.f;
    return {r, g, b, opac};
}


void refresh() {
    float aa = g_opacity.load(std::memory_order_relaxed);
    if (aa < 0.05f) aa = 0.05f;
    if (aa > 1.0f) aa = 1.0f;
    g_chams = {g_r.load(std::memory_order_relaxed), g_g.load(std::memory_order_relaxed),
               g_b.load(std::memory_order_relaxed), aa};
}

void* glProc(const char* name) {
    if (void* p = reinterpret_cast<void*>(eglGetProcAddress(name))) return p;
    static void* h = dlopen("libGLESv2.so", RTLD_NOW);
    if (!h) h = dlopen("libGLESv3.so", RTLD_NOW);
    return h ? dlsym(h, name) : nullptr;
}

// ---- Optional box ESP (off by default; players only) ----
struct Box {
    float minx, miny, minz, maxx, maxy, maxz;
};
std::mutex g_boxMu;
std::vector<Box> g_boxes;
std::mutex g_vpMu;
float g_viewProj[16]{};
std::atomic_bool g_vpValid{false};

using ActorIsPlayerFn = bool (*)(void*);
ActorIsPlayerFn g_isPlayer = nullptr;

using PFN_glUniformMatrix4fv = void (*)(int, int, unsigned char, const float*);
PFN_glUniformMatrix4fv g_glUniformMatrix4fvOrig = nullptr;
bool g_glUniformHooked = false;
std::atomic_bool g_entityRenderArmed{false};

bool finite16(const float* m) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    return true;
}

bool looksLikeViewProj(const float* m) {
    if (!m || !finite16(m)) return false;
    float s = 0.f;
    for (int i = 0; i < 16; ++i) s += std::fabs(m[i]);
    if (s < 2.f || s > 1e5f) return false;
    return std::fabs(m[11]) > 0.05f || std::fabs(m[14]) > 0.05f;
}

bool looksLikeWorldModel(const float* m, float& tx, float& ty, float& tz) {
    if (!m || !finite16(m)) return false;
    if (std::fabs(m[15] - 1.f) > 0.15f) return false;
    tx = m[12];
    ty = m[13];
    tz = m[14];
    if (!std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(tz)) return false;
    float xz = std::sqrt(tx * tx + tz * tz);
    if (xz < 16.f) return false;
    if (std::fabs(ty) < 0.5f && std::fabs(tz) < 0.5f) return false;
    if (ty < -64.f || ty > 400.f) return false;
    if (std::fabs(tx) > 300000.f || std::fabs(tz) > 300000.f) return false;
    return true;
}

void noteWorldBox(float x, float y, float z) {
    Box b{x - 0.4f, y - 0.1f, z - 0.4f, x + 0.4f, y + 1.8f, z + 0.4f};
    std::lock_guard<std::mutex> lock(g_boxMu);
    if (g_boxes.size() < 64) g_boxes.push_back(b);
}

void glUniformMatrix4fvDetour(int location, int count, unsigned char transpose, const float* value) {
    if (value && count >= 1 && g_boxEsp.load(std::memory_order_relaxed)) {
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
        } else if (g_entityRenderArmed.load(std::memory_order_acquire)) {
            float tx, ty, tz;
            if (looksLikeWorldModel(m, tx, ty, tz)) noteWorldBox(tx, ty, tz);
        }
    }
    if (g_glUniformMatrix4fvOrig) g_glUniformMatrix4fvOrig(location, count, transpose, value);
}

bool installMatrixHook() {
    if (g_glUniformHooked) return true;
    void* target = glProc("glUniformMatrix4fv");
    if (!target) return false;
    void* o = nullptr;
    if (pl::memory::hook(target, reinterpret_cast<void*>(&glUniformMatrix4fvDetour), &o) == 0 && o) {
        g_glUniformMatrix4fvOrig = reinterpret_cast<PFN_glUniformMatrix4fv>(o);
        g_glUniformHooked = true;
        logLine("SnowChams: matrix HOOKED (box ESP)");
        return true;
    }
    return false;
}

bool worldToNdc(const float* vp, float x, float y, float z, float& ox, float& oy) {
    float clipX = vp[0] * x + vp[4] * y + vp[8] * z + vp[12];
    float clipY = vp[1] * x + vp[5] * y + vp[9] * z + vp[13];
    float clipW = vp[3] * x + vp[7] * y + vp[11] * z + vp[15];
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
    if (lines.size() < 4) return;
    if (!loadLineGl()) return;
    if (d_glDisable) d_glDisable(GL_DEPTH_TEST);
    if (d_glEnable) d_glEnable(GL_BLEND);
    if (d_glBlendFunc) d_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (d_glLineWidth) d_glLineWidth(2.5f);
    d_glUseProgram(g_prog);
    if (d_glUniform4f && g_uColor >= 0) d_glUniform4f(g_uColor, 1.f, 1.f, 1.f, 0.95f);
    d_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    d_glBufferData(GL_ARRAY_BUFFER, (long)(lines.size() * sizeof(float)), lines.data(), 0x88E4);
    d_glEnableVertexAttribArray((GLuint)g_aPos);
    d_glVertexAttribPointer((GLuint)g_aPos, 2, GL_FLOAT, 0, 0, nullptr);
    d_glDrawArrays(GL_LINES, 0, (GLsizei)(lines.size() / 2));
    d_glUseProgram(0);
}

// ---- FP hand only ----
using RenderFirstPersonFn = void (*)(void*, void*, void*, void*, void*, void*);
RenderFirstPersonFn g_renderFpOriginal = nullptr;

void renderFirstPersonDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!g_renderFpOriginal) return;
    bactro::phase::inFirstPersonHand.store(true, std::memory_order_release);
    g_fpSticky.store(8, std::memory_order_release); // keep APPLY window for following constant setup
    g_renderFpOriginal(self, a1, a2, a3, a4, a5);
}

using SetEntityConstantsFn = void (*)(void*, void*, const Color*, const void*, const void*, const Color*,
                                      const Color*, const Color*, const Color*, const void*, const void*, float,
                                      float, float, float);
SetEntityConstantsFn g_setEntityConstants = nullptr;

void setEntityConstantsDetour(void* entityConstants, void* renderContext, const Color* tileLightColor,
                              const void* tileLightColorUV, const void* blockLightColor, const Color* overlay,
                              const Color* changeColor, const Color* changeColor2, const Color* glintColor,
                              const void* glintUVScale, const void* uvAnim, float uvOffset1, float uvOffset2,
                              float uvRot1, float uvRot2) {
    if (!g_setEntityConstants) return;
    static int s_enter = 0;
    if (s_enter < 8) {
        logLine("HandChams: setEntityConstants ENTER #%d en=%d", s_enter,
                g_enabled.load(std::memory_order_relaxed) ? 1 : 0);
        ++s_enter;
    }
    if (!g_enabled.load(std::memory_order_relaxed)) {
        g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                             overlay, changeColor, changeColor2, glintColor, glintUVScale, uvAnim, uvOffset1,
                             uvOffset2, uvRot1, uvRot2);
        return;
    }
    const bool fp = bactro::phase::inFirstPersonHand.load(std::memory_order_acquire);
    const int sticky = g_fpSticky.load(std::memory_order_acquire);
    const bool handWindow = fp || sticky > 0;
    // Sky shader: tint all item/hand constant setups when enabled
    // Otherwise: gate with targetHand + FP window
    const bool skyOn = g_skyShader.load(std::memory_order_relaxed);
    if (!skyOn && (!g_targetHand.load(std::memory_order_relaxed) || !handWindow)) {
        g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                             overlay, changeColor, changeColor2, glintColor, glintUVScale, uvAnim, uvOffset1,
                             uvOffset2, uvRot1, uvRot2);
        return;
    }
    refresh();
    static Color s_fill{};
    static Color s_edge{};
    if (g_skyShader.load(std::memory_order_relaxed)) {
        s_fill = skyChamsColor();
        s_edge = s_fill;
        s_edge.a = 1.f;
    } else {
        s_fill = g_chams;
        s_edge = g_outline;
        s_edge.a = 1.f;
    }
    const Color* tile = (handWindow ? &s_fill : tileLightColor);
    static int s_app = 0;
    if (s_app < 12) {
        logLine("HandChams: APPLY #%d fp=%d sticky=%d", s_app, fp ? 1 : 0, sticky);
        ++s_app;
    }
    g_setEntityConstants(entityConstants, renderContext, tile, tileLightColorUV, blockLightColor, &s_fill, &s_fill,
                         &s_fill, &s_edge, glintUVScale, uvAnim, uvOffset1, uvOffset2, uvRot1, uvRot2);
}

using SetupActorGlintFn = void (*)(void*, void*, void*, const Color*, const Color*, const Color*, const Color*,
                                   float, float, float, float, const void*);
SetupActorGlintFn g_setupActorGlint = nullptr;
SetupActorGlintFn g_setupGlint = nullptr;

void setupActorGlintDetour(void* screenContext, void* entityContext, void* actor, const Color* overlay,
                           const Color* changeColor, const Color* changeColor2, const Color* glintColor,
                           float uvOffset1, float uvOffset2, float uvRot1, float uvRot2,
                           const void* lightEmissionColor) {
    if (!g_setupActorGlint) return;

    static int s_ag = 0;
    if (s_ag < 6) {
        logLine("HandChams: setupActorGlint ENTER #%d en=%d actor=%p", s_ag,
                g_enabled.load(std::memory_order_relaxed) ? 1 : 0, actor);
        ++s_ag;
    }

    if (g_enabled.load(std::memory_order_relaxed) && g_boxEsp.load(std::memory_order_relaxed) && actor) {
        bool ok = true;
        if (g_playersOnly.load(std::memory_order_relaxed) && g_isPlayer) {
            try {
                ok = g_isPlayer(actor);
            } catch (...) {
                ok = false;
            }
        }
        if (ok) g_entityRenderArmed.store(true, std::memory_order_release);
    }

    // Snow chams: static Color only. Filter by Players / Mobs / Hand.
    if (g_enabled.load(std::memory_order_relaxed)) {
        const bool fp = bactro::phase::inFirstPersonHand.load(std::memory_order_acquire);
        const int sticky = g_fpSticky.load(std::memory_order_acquire);
        const bool handWin = fp || sticky > 0;

        bool doChams = false;
        const bool skyOn = g_skyShader.load(std::memory_order_relaxed);
        if (skyOn) {
            // Sky is hand/items only — never player body/armor (those use real actor*)
            doChams = (actor == nullptr) || handWin;
        } else if (actor == nullptr) {
            doChams = g_targetHand.load(std::memory_order_relaxed);
        } else {
            bool isPl = false;
            if (g_isPlayer) {
                try {
                    isPl = g_isPlayer(actor);
                } catch (...) {
                    isPl = false;
                }
            }
            if (isPl)
                doChams = g_targetPlayers.load(std::memory_order_relaxed);
            else
                doChams = g_targetMobs.load(std::memory_order_relaxed);
        }

        if (doChams) {
            refresh();
            static Color s_fill{};
            static Color s_edge{};
            if (g_skyShader.load(std::memory_order_relaxed)) {
                s_fill = skyChamsColor();
                s_edge = s_fill;
                s_edge.a = 1.f;
                s_edge.r = std::min(1.f, s_edge.r + 0.25f);
                s_edge.g = std::min(1.f, s_edge.g + 0.25f);
                s_edge.b = std::min(1.f, s_edge.b + 0.25f);
            } else {
                s_fill = g_chams;
                s_edge = g_outline;
                s_edge.a = 1.f;
            }

            static int s_gapp = 0;
            if (s_gapp < 10) {
                logLine("HandChams: GLINT APPLY #%d fp=%d actor=%p (Color* only; need mid-hook for outline)", s_gapp,
                        fp ? 1 : 0, actor);
                ++s_gapp;
            }
            g_setupActorGlint(screenContext, entityContext, actor, &s_fill, &s_fill, &s_fill, &s_edge, uvOffset1,
                              uvOffset2, uvRot1, uvRot2, lightEmissionColor);
            return;
        }
    }

    g_setupActorGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor,
                      uvOffset1, uvOffset2, uvRot1, uvRot2, lightEmissionColor);
}

void setupGlintDetour(void* screenContext, void* entityContext, void* actor, const Color* overlay,
                      const Color* changeColor, const Color* changeColor2, const Color* glintColor, float uvOffset1,
                      float uvOffset2, float uvRot1, float uvRot2, const void* lightEmissionColor) {
    if (!g_setupGlint) return;
    // Passthrough only — same crash class as setupActorGlint when colors replaced
    g_setupGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor, uvOffset1,
                 uvOffset2, uvRot1, uvRot2, lightEmissionColor);
}

using SetupFoilFn = void (*)(void*, void*, void*, void*, void*, void*, void*, void*);
SetupFoilFn g_setupFoil = nullptr;
bool g_hookedFoil = false;

void setupFoilDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    if (!g_setupFoil) return;
    static int s_f = 0;
    if (s_f < 5) {
        logLine("HandChams: SetupFoil ENTER #%d en=%d", s_f, g_enabled.load() ? 1 : 0);
        ++s_f;
    }
    // Always call through — foil is enchanted-item path; still useful signal
    g_setupFoil(a0, a1, a2, a3, a4, a5, a6, a7);
}

void tryInstallHooks() {
    void* o = nullptr;

    if (!g_isPlayer) {
        std::uintptr_t addr = bactro::memory::resolve(bactro::memory::SignatureId::ActorIsPlayer);
        if (addr) {
            g_isPlayer = reinterpret_cast<ActorIsPlayerFn>(addr);
            logLine("SnowChams: ActorIsPlayer @%p", reinterpret_cast<void*>(addr));
        }
    }

    // Matrix hook only if box ESP wanted (avoids extra work / crash surface on launch)
    if (g_boxEsp.load(std::memory_order_relaxed)) installMatrixHook();

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
    logLine("SnowChams: hooks fp=%d entity=%d actor=%d glint=%d foil=%d matrix=%d", g_renderFpHooked ? 1 : 0,
            g_hookedEntity ? 1 : 0, g_hookedActor ? 1 : 0, g_hookedGlint ? 1 : 0, g_hookedFoil ? 1 : 0,
            g_glUniformHooked ? 1 : 0);
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
        else if (key == "targetPlayers")
            g_targetPlayers.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "targetMobs")
            g_targetMobs.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "targetHand")
            g_targetHand.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "skyShader")
            g_skyShader.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "skySpeed")
            g_skySpeed.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "skyOpacity")
            g_skyOpacity.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "skyMode")
            g_skyMode.store(static_cast<int>(std::stof(std::string(value))), std::memory_order_relaxed);
        else if (key == "boxEsp") {
            g_boxEsp.store(value == "true" || value == "1", std::memory_order_relaxed);
            if (g_boxEsp.load() && g_enabled.load()) installMatrixHook();
        } else if (key == "playersOnly")
            g_playersOnly.store(value == "true" || value == "1", std::memory_order_relaxed);
        refresh();
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Snow Chams ESP");
    b.description("Stable FP hand white fill + glint outline. No entity recolor (Hive-safe). "
                  "Optional box ESP for players/mobs.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("r", "Red", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("g", "Green", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("b", "Blue", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("opacity", "Opacity", pl::modmenu::ConfigType::SliderFloat, "0.85", "0.05", "1.0", "");
    b.config("targetPlayers", "Players", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("targetMobs", "Mobs / other actors", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("targetHand", "Hand / items / cosmetics", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("skyShader", "Lexora Sky on hand/items", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.config("skySpeed", "Sky speed", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.1", "5.0", "");
    b.config("skyOpacity", "Sky opacity", pl::modmenu::ConfigType::SliderFloat, "0.85", "0.05", "1.0", "");
    b.config("skyMode", "Sky mode 0=plasma 1=aurora 2=midnight 3=nebula", pl::modmenu::ConfigType::SliderFloat, "0", "0", "3", "");
    b.config("boxEsp", "Box ESP", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.config("playersOnly", "ESP players only (off=players+mobs)", pl::modmenu::ConfigType::Toggle, "false", "",
             "", "");
    b.registerModule();
}

void onSignaturesReady() {
    logLine("SnowChams: ready — join world, then enable");
    if (g_enabled.load(std::memory_order_relaxed)) {
        logLine("SnowChams: was ON — installing hooks now");
        tryInstallHooks();
    }
}

void shutdown() { g_enabled.store(false, std::memory_order_release); }

void onPostFrame() {
    if (g_enabled.load(std::memory_order_relaxed)) drawBoxEsp();
    g_entityRenderArmed.store(false, std::memory_order_release);
    bactro::phase::inFirstPersonHand.store(false, std::memory_order_release);
    {
        int s = g_fpSticky.load(std::memory_order_relaxed);
        if (s > 0) g_fpSticky.store(s - 1, std::memory_order_relaxed);
    }
}

} // namespace bactro::handchams
