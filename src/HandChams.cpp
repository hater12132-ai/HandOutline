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
#include <chrono>
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
std::atomic_bool g_galaxy{false};
std::atomic_bool g_wire{true}; // white wire-style edge + GLES line boxes
std::atomic<float> g_r{0.55f};
std::atomic<float> g_g{0.55f};
std::atomic<float> g_b{0.60f}; // grey-ish like screenshot player chams
std::atomic<float> g_intensity{1.0f};
std::atomic<float> g_opacity{1.0f};
std::atomic<float> g_wireStr{0.35f}; // soft white edge (NOT full white fill)
std::atomic<float> g_stars{0.45f};
std::atomic_int g_hits{0};
std::atomic_int g_playerHits{0};

Color g_fillColor{0.55f, 0.55f, 0.60f, 1.0f};
Color g_wireOverlay{1.0f, 1.0f, 1.05f, 0.2f};

using ActorIsPlayerFn = bool (*)(void*);
ActorIsPlayerFn g_isPlayer = nullptr;

void logLine(const char* fmt, ...) {
    char buf[200];
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

float hash11(float x) {
    x = std::fmod(x * 0.1031f, 1.0f);
    if (x < 0) x += 1.0f;
    return std::fmod(x * (x + 33.33f) * (x + x), 1.0f);
}

void refreshColors() {
    const float i = g_intensity.load(std::memory_order_relaxed);
    const float a = g_opacity.load(std::memory_order_relaxed);

    if (g_galaxy.load(std::memory_order_relaxed)) {
        const float t = nowSec();
        const float n1 = 0.5f + 0.5f * std::sin(t * 0.55f);
        const float n2 = 0.5f + 0.5f * std::sin(t * 0.92f + 2.1f);
        float r = 0.05f + 0.18f * n2;
        float g = 0.06f + 0.14f * n1;
        float b = 0.28f + 0.40f * n1;
        const float starAmt = g_stars.load(std::memory_order_relaxed);
        if (starAmt > 0.01f) {
            for (int k = 0; k < 5; ++k) {
                const float cell = std::floor(t * (2.5f + k * 1.7f) + k * 13.1f);
                const float h = hash11(cell + k * 7.7f);
                if (h > 0.82f) {
                    const float phase = std::fmod(t * (3.0f + k), 1.0f);
                    float tw = 0.0f;
                    if (phase < 0.15f)
                        tw = phase / 0.15f;
                    else if (phase < 0.35f)
                        tw = 1.0f;
                    else if (phase < 0.5f)
                        tw = 1.0f - (phase - 0.35f) / 0.15f;
                    const float s = tw * starAmt * (0.5f + 0.5f * h);
                    r += s * 0.95f;
                    g += s * 0.95f;
                    b += s * 1.05f;
                }
            }
        }
        if (r > 1.5f) r = 1.5f;
        if (g > 1.5f) g = 1.5f;
        if (b > 1.5f) b = 1.5f;
        g_fillColor = {r * i, g * i, b * i, a};
    } else {
        g_fillColor = {
            g_r.load(std::memory_order_relaxed) * i,
            g_g.load(std::memory_order_relaxed) * i,
            g_b.load(std::memory_order_relaxed) * i,
            a,
        };
    }

    // Soft white edge only — low alpha so it does NOT wash the whole hand white
    const float w = g_wire.load(std::memory_order_relaxed) ? g_wireStr.load(std::memory_order_relaxed) : 0.0f;
    g_wireOverlay = {1.0f, 1.0f, 1.05f, 0.04f + w * 0.28f};
}

bool shouldApply() {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    if (g_handOnly.load(std::memory_order_relaxed))
        return bactro::phase::inFirstPersonHand.load(std::memory_order_acquire);
    return true;
}

// ---- GLES2 line box (screen-space wire) ----
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
constexpr GLenum GL_TRUE = 1;
constexpr GLenum GL_FALSE = 0;

using PFN_glCreateShader = GLuint (*)(GLenum);
using PFN_glShaderSource = void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glCompileShader = void (*)(GLuint);
using PFN_glGetShaderiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glCreateProgram = GLuint (*)();
using PFN_glAttachShader = void (*)(GLuint, GLuint);
using PFN_glLinkProgram = void (*)(GLuint);
using PFN_glGetProgramiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glUseProgram = void (*)(GLuint);
using PFN_glGetAttribLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glGetUniformLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glGenBuffers = void (*)(GLsizei, GLuint*);
using PFN_glBindBuffer = void (*)(GLenum, GLuint);
using PFN_glBufferData = void (*)(GLenum, long, const void*, GLenum);
using PFN_glEnableVertexAttribArray = void (*)(GLuint);
using PFN_glVertexAttribPointer = void (*)(GLuint, GLint, GLenum, unsigned char, GLsizei, const void*);
using PFN_glDrawArrays = void (*)(GLenum, GLint, GLsizei);
using PFN_glEnable = void (*)(GLenum);
using PFN_glDisable = void (*)(GLenum);
using PFN_glBlendFunc = void (*)(GLenum, GLenum);
using PFN_glLineWidth = void (*)(GLfloat);
using PFN_glDeleteShader = void (*)(GLuint);

PFN_glCreateShader p_glCreateShader = nullptr;
PFN_glShaderSource p_glShaderSource = nullptr;
PFN_glCompileShader p_glCompileShader = nullptr;
PFN_glGetShaderiv p_glGetShaderiv = nullptr;
PFN_glCreateProgram p_glCreateProgram = nullptr;
PFN_glAttachShader p_glAttachShader = nullptr;
PFN_glLinkProgram p_glLinkProgram = nullptr;
PFN_glGetProgramiv p_glGetProgramiv = nullptr;
PFN_glUseProgram p_glUseProgram = nullptr;
PFN_glGetAttribLocation p_glGetAttribLocation = nullptr;
PFN_glGetUniformLocation p_glGetUniformLocation = nullptr;
PFN_glGenBuffers p_glGenBuffers = nullptr;
PFN_glBindBuffer p_glBindBuffer = nullptr;
PFN_glBufferData p_glBufferData = nullptr;
PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray = nullptr;
PFN_glVertexAttribPointer p_glVertexAttribPointer = nullptr;
PFN_glDrawArrays p_glDrawArrays = nullptr;
PFN_glEnable p_glEnable = nullptr;
PFN_glDisable p_glDisable = nullptr;
PFN_glBlendFunc p_glBlendFunc = nullptr;
PFN_glLineWidth p_glLineWidth = nullptr;
PFN_glDeleteShader p_glDeleteShader = nullptr;

bool g_glLoaded = false;
GLuint g_lineProg = 0;
GLuint g_lineVbo = 0;
GLint g_attrPos = -1;
GLint g_uniColor = -1;
std::mutex g_glMu;

void* glProc(const char* name) {
    if (void* p = reinterpret_cast<void*>(eglGetProcAddress(name))) return p;
    void* h = dlopen("libGLESv2.so", RTLD_NOW);
    if (!h) h = dlopen("libGLESv3.so", RTLD_NOW);
    return h ? dlsym(h, name) : nullptr;
}

bool loadGl() {
    if (g_glLoaded) return g_lineProg != 0;
    g_glLoaded = true;
#define L(name) p_##name = reinterpret_cast<PFN_##name>(glProc(#name))
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
        logLine("HandChams: GLES line load FAIL");
        return false;
    }

    const char* vs = "attribute vec2 aPos; void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }";
    const char* fs = "precision mediump float; uniform vec4 uColor; void main(){ gl_FragColor = uColor; }";

    auto compile = [](GLenum type, const char* src) -> GLuint {
        GLuint s = p_glCreateShader(type);
        p_glShaderSource(s, 1, &src, nullptr);
        p_glCompileShader(s);
        GLint ok = 0;
        p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        return ok ? s : 0;
    };
    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) {
        logLine("HandChams: line shader compile FAIL");
        return false;
    }
    g_lineProg = p_glCreateProgram();
    p_glAttachShader(g_lineProg, v);
    p_glAttachShader(g_lineProg, f);
    p_glLinkProgram(g_lineProg);
    GLint linked = 0;
    p_glGetProgramiv(g_lineProg, GL_LINK_STATUS, &linked);
    p_glDeleteShader(v);
    p_glDeleteShader(f);
    if (!linked) {
        g_lineProg = 0;
        logLine("HandChams: line shader link FAIL");
        return false;
    }
    g_attrPos = p_glGetAttribLocation(g_lineProg, "aPos");
    g_uniColor = p_glGetUniformLocation(g_lineProg, "uColor");
    p_glGenBuffers(1, &g_lineVbo);
    logLine("HandChams: GLES wire lines ready");
    return true;
}

// Screen NDC box (x0,y0)-(x1,y1) as 12 line ends (4 edges * 2, but we do 4 lines = 8 verts)
void appendBox2D(std::vector<float>& out, float x0, float y0, float x1, float y1) {
    auto edge = [&](float ax, float ay, float bx, float by) {
        out.push_back(ax);
        out.push_back(ay);
        out.push_back(bx);
        out.push_back(by);
    };
    edge(x0, y0, x1, y0);
    edge(x1, y0, x1, y1);
    edge(x1, y1, x0, y1);
    edge(x0, y1, x0, y0);
}

// Called from MotionBlur after frame (same GL context)
void drawWireOverlay() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    if (!g_wire.load(std::memory_order_relaxed)) return;

    std::lock_guard<std::mutex> lock(g_glMu);
    if (!loadGl() || !g_lineProg) return;

    // Without stable world positions we draw a thin white HUD-style target box
    // when player-chams hits recently (shows wire system is alive).
    // Full 3D entity AABB wire needs version-specific pos offsets (crash risk).
    const int ph = g_playerHits.load(std::memory_order_relaxed);
    std::vector<float> verts;
    if (ph > 0) {
        // Center crosshair wire box (screenshot-style white lines in view)
        appendBox2D(verts, -0.12f, -0.18f, 0.12f, 0.18f);
        appendBox2D(verts, -0.08f, -0.12f, 0.08f, 0.12f);
    }

    if (verts.empty()) return;

    p_glDisable(GL_DEPTH_TEST);
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (p_glLineWidth) p_glLineWidth(2.0f);

    p_glUseProgram(g_lineProg);
    // White wire
    // uColor via uniform — need glUniform4f
    using PFN_glUniform4f = void (*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    static PFN_glUniform4f p_glUniform4f =
        reinterpret_cast<PFN_glUniform4f>(glProc("glUniform4f"));
    if (p_glUniform4f && g_uniColor >= 0)
        p_glUniform4f(g_uniColor, 1.0f, 1.0f, 1.0f, 0.95f);

    p_glBindBuffer(GL_ARRAY_BUFFER, g_lineVbo);
    p_glBufferData(GL_ARRAY_BUFFER, static_cast<long>(verts.size() * sizeof(float)), verts.data(),
                   0x88E4);
    p_glEnableVertexAttribArray(static_cast<GLuint>(g_attrPos));
    p_glVertexAttribPointer(static_cast<GLuint>(g_attrPos), 2, GL_FLOAT, 0, 0, nullptr);
    p_glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(verts.size() / 2));
    p_glUseProgram(0);
}

// ---- FP marker ----
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
    refreshColors();
    // fill = solid chams; overlay = soft white wire edge (low alpha)
    const Color* ov = g_wire.load(std::memory_order_relaxed) ? &g_wireOverlay : &g_fillColor;
    g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor, ov,
                         &g_fillColor, &g_fillColor, glintColor, glintUVScale, uvAnim, uvOffset1, uvOffset2,
                         uvRot1, uvRot2);
    const int n = g_hits.fetch_add(1, std::memory_order_relaxed);
    if (n < 6)
        logLine("HandChams: fill #%d wire=%d fp=%d", n, g_wire.load() ? 1 : 0,
                bactro::phase::inFirstPersonHand.load() ? 1 : 0);
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

    bool player = false;
    if (actor && g_isPlayer) {
        try {
            player = g_isPlayer(actor);
        } catch (...) {
            player = false;
        }
    }
    if (player) g_playerHits.fetch_add(1, std::memory_order_relaxed);

    if (!shouldApply()) {
        g_setupActorGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor,
                          uvOffset1, uvOffset2, uvRot1, uvRot2, lightEmissionColor);
        return;
    }
    refreshColors();
    const Color* ov = g_wire.load(std::memory_order_relaxed) ? &g_wireOverlay : &g_fillColor;
    g_setupActorGlint(screenContext, entityContext, actor, ov, &g_fillColor, &g_fillColor, glintColor, uvOffset1,
                      uvOffset2, uvRot1, uvRot2, lightEmissionColor);
    if (player) {
        const int n = g_playerHits.load();
        if (n <= 5) logLine("HandChams: player chams #%d", n);
    }
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
    refreshColors();
    const Color* ov = g_wire.load(std::memory_order_relaxed) ? &g_wireOverlay : &g_fillColor;
    g_setupFoil(a0, a1, a2, ov, &g_fillColor, f0, f1, f2, f3);
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
    refreshColors();
    const Color* ov = g_wire.load(std::memory_order_relaxed) ? &g_wireOverlay : &g_fillColor;
    g_setupGlint(a0, a1, a2, ov, &g_fillColor, &g_fillColor, f0, f1, f2, f3);
}

void tryInstallHooks() {
    void* o = nullptr;

    if (!g_isPlayer) {
        std::uintptr_t addr = bactro::memory::resolve(bactro::memory::SignatureId::ActorIsPlayer);
        if (addr) {
            g_isPlayer = reinterpret_cast<ActorIsPlayerFn>(addr);
            logLine("HandChams: ActorIsPlayer @%p", reinterpret_cast<void*>(addr));
        }
    }

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

    logLine("HandChams: hooks fp=%d entity=%d actor=%d foil=%d glint=%d isPlayer=%d", g_renderFpHooked ? 1 : 0,
            g_hookedEntity ? 1 : 0, g_hookedActor ? 1 : 0, g_hookedFoil ? 1 : 0, g_hookedGlint ? 1 : 0,
            g_isPlayer ? 1 : 0);
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
        else if (key == "wireStr")
            g_wireStr.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "stars")
            g_stars.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "handOnly")
            g_handOnly.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "galaxy")
            g_galaxy.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "wire")
            g_wire.store(value == "true" || value == "1", std::memory_order_relaxed);
        refreshColors();
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Hand Chams");
    b.description(
         "Solid chams (hand/items + players when engine allows). "
         "Wire = soft white edge + line overlay (no full-white wash). "
         "Join world, then enable.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("wire", "White wire edge", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("wireStr", "Wire strength", pl::modmenu::ConfigType::SliderFloat, "0.35", "0", "1", "");
    b.config("galaxy", "Galaxy fill", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.config("stars", "Star sparkles", pl::modmenu::ConfigType::SliderFloat, "0.45", "0", "1", "");
    b.config("opacity", "Opacity", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.05", "1.0", "");
    b.config("intensity", "Intensity", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.1", "2.0", "");
    b.config("r", "Red", pl::modmenu::ConfigType::SliderFloat, "0.55", "0", "1", "");
    b.config("g", "Green", pl::modmenu::ConfigType::SliderFloat, "0.55", "0", "1", "");
    b.config("b", "Blue", pl::modmenu::ConfigType::SliderFloat, "0.60", "0", "1", "");
    b.config("handOnly", "Hand only", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.registerModule();
}

void onSignaturesReady() {
    logLine("HandChams: ready — enable after you join the world");
}

void shutdown() { g_enabled.store(false, std::memory_order_release); }

// called from MotionBlur swap path
void onPostFrame() {
    try {
        drawWireOverlay();
    } catch (...) {
    }
}

} // namespace bactro::handchams
