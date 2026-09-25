#include "bactro/PhaseHud.hpp"
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

#define PH_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

// Minimal GLES2 without system headers
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLchar = char;
using GLboolean = unsigned char;

constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLenum GL_TRIANGLE_FAN = 0x0006;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_FALSE = 0;
constexpr GLenum GL_TRUE = 1;

namespace bactro::phasehud {
namespace {

constexpr const char* kModuleId = "bactro.phasehud";

std::atomic_bool g_enabled{true};
std::atomic<float> g_scale{0.22f};   // size of card vs screen width
std::atomic<float> g_y{ -0.72f};    // NDC y center (bottom)
std::atomic<float> g_opacity{0.92f};

std::mutex g_mu;
bool g_ready = false;
bool g_hooked = false;
int g_drawLog = 0;

GLuint g_prog = 0;
GLuint g_vbo = 0;
GLint g_aPos = -1;
GLint g_uTime = -1;
GLint g_uCenter = -1;
GLint g_uScale = -1;
GLint g_uOpacity = -1;
GLint g_uMode = -1; // 0 = fill, 1 = border

using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
EglSwapBuffersFn g_swapOriginal = nullptr;

// GLES procs
void* (*p_eglGetProcAddress)(const char*) = nullptr;
GLuint (*p_glCreateShader)(GLenum) = nullptr;
void (*p_glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
void (*p_glCompileShader)(GLuint) = nullptr;
void (*p_glGetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
void (*p_glDeleteShader)(GLuint) = nullptr;
GLuint (*p_glCreateProgram)() = nullptr;
void (*p_glAttachShader)(GLuint, GLuint) = nullptr;
void (*p_glLinkProgram)(GLuint) = nullptr;
void (*p_glGetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
void (*p_glUseProgram)(GLuint) = nullptr;
GLint (*p_glGetAttribLocation)(GLuint, const GLchar*) = nullptr;
GLint (*p_glGetUniformLocation)(GLuint, const GLchar*) = nullptr;
void (*p_glGenBuffers)(GLsizei, GLuint*) = nullptr;
void (*p_glBindBuffer)(GLenum, GLuint) = nullptr;
void (*p_glBufferData)(GLenum, long, const void*, GLenum) = nullptr;
void (*p_glEnableVertexAttribArray)(GLuint) = nullptr;
void (*p_glDisableVertexAttribArray)(GLuint) = nullptr;
void (*p_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
void (*p_glUniform1f)(GLint, GLfloat) = nullptr;
void (*p_glUniform2f)(GLint, GLfloat, GLfloat) = nullptr;
void (*p_glDrawArrays)(GLenum, GLint, GLsizei) = nullptr;
void (*p_glEnable)(GLenum) = nullptr;
void (*p_glDisable)(GLenum) = nullptr;
void (*p_glBlendFunc)(GLenum, GLenum) = nullptr;
void (*p_glGetIntegerv)(GLenum, GLint*) = nullptr;
int (*p_glGetError)() = nullptr;

void logLine(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    PH_LOGI("%s", buf);
}

void* loadProc(const char* name) {
    void* p = nullptr;
    if (p_eglGetProcAddress) p = p_eglGetProcAddress(name);
    if (!p) {
        void* lib = dlopen("libGLESv2.so", RTLD_NOW);
        if (lib) p = dlsym(lib, name);
    }
    return p;
}

bool loadGl() {
    if (p_glCreateShader) return true;
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    if (egl) p_eglGetProcAddress = reinterpret_cast<decltype(p_eglGetProcAddress)>(dlsym(egl, "eglGetProcAddress"));
#define L(name, var) var = reinterpret_cast<decltype(var)>(loadProc(name))
    L("glCreateShader", p_glCreateShader);
    L("glShaderSource", p_glShaderSource);
    L("glCompileShader", p_glCompileShader);
    L("glGetShaderiv", p_glGetShaderiv);
    L("glDeleteShader", p_glDeleteShader);
    L("glCreateProgram", p_glCreateProgram);
    L("glAttachShader", p_glAttachShader);
    L("glLinkProgram", p_glLinkProgram);
    L("glGetProgramiv", p_glGetProgramiv);
    L("glUseProgram", p_glUseProgram);
    L("glGetAttribLocation", p_glGetAttribLocation);
    L("glGetUniformLocation", p_glGetUniformLocation);
    L("glGenBuffers", p_glGenBuffers);
    L("glBindBuffer", p_glBindBuffer);
    L("glBufferData", p_glBufferData);
    L("glEnableVertexAttribArray", p_glEnableVertexAttribArray);
    L("glDisableVertexAttribArray", p_glDisableVertexAttribArray);
    L("glVertexAttribPointer", p_glVertexAttribPointer);
    L("glUniform1f", p_glUniform1f);
    L("glUniform2f", p_glUniform2f);
    L("glDrawArrays", p_glDrawArrays);
    L("glEnable", p_glEnable);
    L("glDisable", p_glDisable);
    L("glBlendFunc", p_glBlendFunc);
    L("glGetIntegerv", p_glGetIntegerv);
#undef L
    return p_glCreateShader && p_glUseProgram && p_glDrawArrays;
}

static constexpr const char* kVS = R"(
attribute vec2 aPos;
varying vec2 vUv;
uniform vec2 uCenter;
uniform float uScale;
void main() {
    // aPos in unit pentagon space [-1,1]
    vec2 p = aPos * uScale;
    // aspect-ish: assume ~9:16 phone → squash x slightly via scale only
    gl_Position = vec4(uCenter + p, 0.0, 1.0);
    vUv = aPos * 0.5 + 0.5;
}
)";

// Galaxy fill + optional white border mode
static constexpr const char* kFS = R"(
precision mediump float;
varying vec2 vUv;
uniform float uTime;
uniform float uOpacity;
uniform float uMode; // 0 fill, 1 border mask helper (unused if separate draw)
void main() {
    vec2 uv = vUv;
    float t = uTime;
    // procedural stars / nebula
    vec2 guv = uv * 12.0 + vec2(t * 0.07, t * 0.04);
    float n1 = fract(sin(dot(floor(guv), vec2(12.9898, 78.233))) * 43758.5453);
    float n2 = fract(sin(dot(floor(guv * 1.8 + 3.1), vec2(39.346, 11.135))) * 23421.631);
    float stars = step(0.92, n1) * (0.5 + 0.5 * sin(t * 4.0 + n1 * 40.0));
    stars += step(0.95, n2) * 0.45;
    vec3 galaxy = vec3(0.05, 0.08, 0.28) + vec3(0.12, 0.15, 0.45) * n2 + vec3(stars);
    gl_FragColor = vec4(galaxy, uOpacity);
}
)";

static constexpr const char* kFSBorder = R"(
precision mediump float;
uniform float uOpacity;
void main() {
    gl_FragColor = vec4(1.0, 1.0, 1.05, uOpacity);
}
)";

GLuint g_progBorder = 0;
GLint g_aPosB = -1;
GLint g_uCenterB = -1;
GLint g_uScaleB = -1;
GLint g_uOpacityB = -1;

bool compile(GLenum type, const char* src, GLuint& out) {
    out = p_glCreateShader(type);
    p_glShaderSource(out, 1, &src, nullptr);
    p_glCompileShader(out);
    GLint ok = 0;
    p_glGetShaderiv(out, GL_COMPILE_STATUS, &ok);
    return ok != 0;
}

bool initGl() {
    if (g_ready) return true;
    if (!loadGl()) {
        logLine("PhaseHud: GLES load fail");
        return false;
    }
    GLuint vs = 0, fs = 0, fsb = 0;
    if (!compile(GL_VERTEX_SHADER, kVS, vs) || !compile(GL_FRAGMENT_SHADER, kFS, fs)) {
        logLine("PhaseHud: shader compile fail (fill)");
        return false;
    }
    g_prog = p_glCreateProgram();
    p_glAttachShader(g_prog, vs);
    p_glAttachShader(g_prog, fs);
    p_glLinkProgram(g_prog);
    GLint linked = 0;
    p_glGetProgramiv(g_prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        logLine("PhaseHud: link fail fill");
        return false;
    }
    g_aPos = p_glGetAttribLocation(g_prog, "aPos");
    g_uTime = p_glGetUniformLocation(g_prog, "uTime");
    g_uCenter = p_glGetUniformLocation(g_prog, "uCenter");
    g_uScale = p_glGetUniformLocation(g_prog, "uScale");
    g_uOpacity = p_glGetUniformLocation(g_prog, "uOpacity");

    if (!compile(GL_FRAGMENT_SHADER, kFSBorder, fsb)) {
        logLine("PhaseHud: border shader fail");
        return false;
    }
    g_progBorder = p_glCreateProgram();
    p_glAttachShader(g_progBorder, vs);
    p_glAttachShader(g_progBorder, fsb);
    p_glLinkProgram(g_progBorder);
    p_glGetProgramiv(g_progBorder, GL_LINK_STATUS, &linked);
    if (!linked) {
        logLine("PhaseHud: link fail border");
        return false;
    }
    g_aPosB = p_glGetAttribLocation(g_progBorder, "aPos");
    g_uCenterB = p_glGetUniformLocation(g_progBorder, "uCenter");
    g_uScaleB = p_glGetUniformLocation(g_progBorder, "uScale");
    g_uOpacityB = p_glGetUniformLocation(g_progBorder, "uOpacity");

    // Unit pentagon (point up), center 0,0
    // angles: -90 + i*72 deg in NDC-ish
    GLfloat verts[7 * 2]; // fan: center + 5 verts + repeat first
    verts[0] = 0.f;
    verts[1] = 0.f;
    for (int i = 0; i < 5; ++i) {
        float ang = (-90.f + i * 72.f) * 3.14159265f / 180.f;
        verts[(i + 1) * 2 + 0] = std::cos(ang);
        verts[(i + 1) * 2 + 1] = std::sin(ang);
    }
    verts[12] = verts[2];
    verts[13] = verts[3];

    p_glGenBuffers(1, &g_vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    g_ready = true;
    logLine("PhaseHud: GL ready (2D galaxy pentagon)");
    return true;
}

void drawHud() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_mu);
    if (!initGl()) return;

    const float scale = g_scale.load(std::memory_order_relaxed);
    const float y = g_y.load(std::memory_order_relaxed);
    const float op = g_opacity.load(std::memory_order_relaxed);
    static float t = 0.f;
    t += 0.016f;

    // Save minimal state we touch
    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_CULL_FACE);
    p_glDisable(GL_SCISSOR_TEST);
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

    // BORDER (slightly larger white) then FILL on top
    p_glUseProgram(g_progBorder);
    if (g_uCenterB >= 0) p_glUniform2f(g_uCenterB, 0.f, y);
    if (g_uScaleB >= 0) p_glUniform1f(g_uScaleB, scale * 1.05f);
    if (g_uOpacityB >= 0) p_glUniform1f(g_uOpacityB, op);
    if (g_aPosB >= 0) {
        p_glEnableVertexAttribArray(static_cast<GLuint>(g_aPosB));
        p_glVertexAttribPointer(static_cast<GLuint>(g_aPosB), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }
    p_glDrawArrays(GL_TRIANGLE_FAN, 0, 7);

    // FILL galaxy
    p_glUseProgram(g_prog);
    if (g_uTime >= 0) p_glUniform1f(g_uTime, t);
    if (g_uCenter >= 0) p_glUniform2f(g_uCenter, 0.f, y);
    if (g_uScale >= 0) p_glUniform1f(g_uScale, scale);
    if (g_uOpacity >= 0) p_glUniform1f(g_uOpacity, op);
    if (g_aPos >= 0) {
        p_glEnableVertexAttribArray(static_cast<GLuint>(g_aPos));
        p_glVertexAttribPointer(static_cast<GLuint>(g_aPos), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }
    p_glDrawArrays(GL_TRIANGLE_FAN, 0, 7);

    if (g_drawLog < 3) {
        logLine("PhaseHud: draw #%d scale=%.2f y=%.2f", g_drawLog, scale, y);
        ++g_drawLog;
    }
}

EGLBoolean swapDetour(EGLDisplay dpy, EGLSurface surf) {
    drawHud();
    if (g_swapOriginal) return g_swapOriginal(dpy, surf);
    return EGL_FALSE;
}

void tryHook() {
    if (g_hooked) return;
    void* swap = nullptr;
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    if (egl) {
        p_eglGetProcAddress = reinterpret_cast<decltype(p_eglGetProcAddress)>(dlsym(egl, "eglGetProcAddress"));
        if (p_eglGetProcAddress) swap = p_eglGetProcAddress("eglSwapBuffers");
        if (!swap) swap = dlsym(egl, "eglSwapBuffers");
    }
    if (!swap) {
        logLine("PhaseHud: eglSwapBuffers not found");
        return;
    }
    void* o = nullptr;
    // Install AFTER MotionBlur so we draw on top path: us -> motionblur -> real swap
    if (pl::memory::hook(swap, reinterpret_cast<void*>(&swapDetour), &o) == 0 && o) {
        g_swapOriginal = reinterpret_cast<EglSwapBuffersFn>(o);
        g_hooked = true;
        logLine("PhaseHud: eglSwapBuffers hooked");
    } else if (o) {
        g_swapOriginal = reinterpret_cast<EglSwapBuffersFn>(o);
        g_hooked = true;
        logLine("PhaseHud: eglSwapBuffers hooked (nonzero rc)");
    } else {
        logLine("PhaseHud: hook FAILED");
    }
}

void onToggle(std::string_view, bool en) {
    g_enabled.store(en, std::memory_order_release);
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "scale") g_scale.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "y") g_y.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "opacity") g_opacity.store(std::stof(std::string(value)), std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Phase HUD");
    b.description(
         "2D Phase-style galaxy pentagon overlay (bottom center). "
         "Pure visual — not a 3D item material. Match the HUD card look.")
        .defaultEnabled(true)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("scale", "Card size", pl::modmenu::ConfigType::SliderFloat, "0.22", "0.10", "0.45", "");
    b.config("y", "Vertical position (NDC)", pl::modmenu::ConfigType::SliderFloat, "-0.72", "-0.95", "-0.20", "");
    b.config("opacity", "Opacity", pl::modmenu::ConfigType::SliderFloat, "0.92", "0.3", "1.0", "");
    b.registerModule();
}

void onSignaturesReady() { tryHook(); }

void shutdown() {
    g_enabled.store(false, std::memory_order_release);
}

} // namespace bactro::phasehud
