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
constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_TRIANGLE_FAN = 0x0006;
constexpr GLenum GL_LINE_LOOP = 0x0002;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_FALSE = 0;
constexpr GLenum GL_TRUE = 1;
constexpr GLenum GL_BLEND_SRC_ALPHA = 0x80CB;

namespace bactro::phasehud {
namespace {

constexpr const char* kModuleId = "bactro.phasehud";

std::atomic_bool g_enabled{false};
std::atomic<float> g_scale{0.18f};
std::atomic<float> g_x{0.62f};
std::atomic<float> g_y{-0.48f};
std::atomic<float> g_opacity{0.95f};

std::mutex g_mu;
bool g_ready = false;
bool g_hooked = false;
bool g_failed = false;
int g_drawLog = 0;

GLuint g_prog = 0;
GLuint g_vboFill = 0;
GLuint g_vboBorder = 0;
GLint g_aPos = -1;
GLint g_aCol = -1;

using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
EglSwapBuffersFn g_swapOriginal = nullptr;

void* (*p_eglGetProcAddress)(const char*) = nullptr;
GLuint (*p_glCreateShader)(GLenum) = nullptr;
void (*p_glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
void (*p_glCompileShader)(GLuint) = nullptr;
void (*p_glGetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
void (*p_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
void (*p_glDeleteShader)(GLuint) = nullptr;
GLuint (*p_glCreateProgram)() = nullptr;
void (*p_glAttachShader)(GLuint, GLuint) = nullptr;
void (*p_glLinkProgram)(GLuint) = nullptr;
void (*p_glGetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
void (*p_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
void (*p_glUseProgram)(GLuint) = nullptr;
GLint (*p_glGetAttribLocation)(GLuint, const GLchar*) = nullptr;
void (*p_glGenBuffers)(GLsizei, GLuint*) = nullptr;
void (*p_glBindBuffer)(GLenum, GLuint) = nullptr;
void (*p_glBufferData)(GLenum, GLsizei, const void*, GLenum) = nullptr;
void (*p_glEnableVertexAttribArray)(GLuint) = nullptr;
void (*p_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
void (*p_glDrawArrays)(GLenum, GLint, GLsizei) = nullptr;
void (*p_glEnable)(GLenum) = nullptr;
void (*p_glDisable)(GLenum) = nullptr;
void (*p_glBlendFunc)(GLenum, GLenum) = nullptr;
void (*p_glLineWidth)(GLfloat) = nullptr;

void logLine(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    PH_LOGI("%s", buf);
}

void* loadProc(const char* name) {
    // Prefer eglGetProcAddress (same as MotionBlur)
    void* p = reinterpret_cast<void*>(eglGetProcAddress(name));
    if (p) return p;
    if (p_eglGetProcAddress) {
        p = p_eglGetProcAddress(name);
        if (p) return p;
    }
    void* lib = dlopen("libGLESv2.so", RTLD_NOW);
    if (!lib) lib = dlopen("libGLESv3.so", RTLD_NOW);
    return lib ? dlsym(lib, name) : nullptr;
}

bool loadGl() {
    if (p_glCreateShader) return true;
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    if (egl)
        p_eglGetProcAddress =
            reinterpret_cast<decltype(p_eglGetProcAddress)>(dlsym(egl, "eglGetProcAddress"));
#define L(n, v) v = reinterpret_cast<decltype(v)>(loadProc(n))
    L("glCreateShader", p_glCreateShader);
    L("glShaderSource", p_glShaderSource);
    L("glCompileShader", p_glCompileShader);
    L("glGetShaderiv", p_glGetShaderiv);
    L("glGetShaderInfoLog", p_glGetShaderInfoLog);
    L("glDeleteShader", p_glDeleteShader);
    L("glCreateProgram", p_glCreateProgram);
    L("glAttachShader", p_glAttachShader);
    L("glLinkProgram", p_glLinkProgram);
    L("glGetProgramiv", p_glGetProgramiv);
    L("glGetProgramInfoLog", p_glGetProgramInfoLog);
    L("glUseProgram", p_glUseProgram);
    L("glGetAttribLocation", p_glGetAttribLocation);
    L("glGenBuffers", p_glGenBuffers);
    L("glBindBuffer", p_glBindBuffer);
    L("glBufferData", p_glBufferData);
    L("glEnableVertexAttribArray", p_glEnableVertexAttribArray);
    L("glVertexAttribPointer", p_glVertexAttribPointer);
    L("glDrawArrays", p_glDrawArrays);
    L("glEnable", p_glEnable);
    L("glDisable", p_glDisable);
    L("glBlendFunc", p_glBlendFunc);
    L("glLineWidth", p_glLineWidth);
#undef L
    return p_glCreateShader && p_glUseProgram && p_glDrawArrays && p_glShaderSource;
}

// Same style as MotionBlur — GLES2 attribute/varying, no ES3
static constexpr const char* kVS = R"(
attribute vec4 aPosition;
attribute vec4 aColor;
varying vec4 vColor;
void main() {
    gl_Position = aPosition;
    vColor = aColor;
}
)";

static constexpr const char* kFS = R"(
precision mediump float;
varying vec4 vColor;
void main() {
    gl_FragColor = vColor;
}
)";

bool compileShader(GLenum type, const char* src, GLuint& out) {
    out = p_glCreateShader(type);
    p_glShaderSource(out, 1, &src, nullptr);
    p_glCompileShader(out);
    GLint ok = 0;
    p_glGetShaderiv(out, GL_COMPILE_STATUS, &ok);
    if (!ok && p_glGetShaderInfoLog) {
        char log[256];
        p_glGetShaderInfoLog(out, 255, nullptr, log);
        log[255] = 0;
        logLine("PhaseHud: compile log: %s", log);
    }
    return ok != 0;
}

void buildGeometry(float scale, float x, float y, float opacity) {
    // Interleaved pos.xy + color.rgba — 6 floats per vertex
    // Fill: triangle fan center + 5 verts
    GLfloat fill[7 * 6];
    // center — deep navy (screen position over FP hand)
    fill[0] = x;
    fill[1] = y;
    fill[2] = 0.06f;
    fill[3] = 0.10f;
    fill[4] = 0.32f;
    fill[5] = opacity;
    for (int i = 0; i < 5; ++i) {
        float ang = (-90.f + static_cast<float>(i) * 72.f) * 3.14159265f / 180.f;
        float px = std::cos(ang) * scale;
        float py = std::sin(ang) * scale * 1.15f; // slight vertical stretch
        int o = (i + 1) * 6;
        fill[o + 0] = x + px;
        fill[o + 1] = y + py;
        // outer verts — galaxy blue with star-ish variation
        float star = (i % 2 == 0) ? 0.35f : 0.15f;
        fill[o + 2] = 0.08f + star * 0.15f;
        fill[o + 3] = 0.12f + star * 0.20f;
        fill[o + 4] = 0.38f + star * 0.45f;
        fill[o + 5] = opacity;
    }
    // close fan
    fill[6 * 6 + 0] = fill[6];
    fill[6 * 6 + 1] = fill[7];
    fill[6 * 6 + 2] = fill[8];
    fill[6 * 6 + 3] = fill[9];
    fill[6 * 6 + 4] = fill[10];
    fill[6 * 6 + 5] = fill[11];

    // Border: 5 verts white
    GLfloat border[5 * 6];
    for (int i = 0; i < 5; ++i) {
        float ang = (-90.f + static_cast<float>(i) * 72.f) * 3.14159265f / 180.f;
        float px = std::cos(ang) * scale * 1.04f;
        float py = std::sin(ang) * scale * 1.15f * 1.04f;
        int o = i * 6;
        border[o + 0] = x + px;
        border[o + 1] = y + py;
        border[o + 2] = 1.0f;
        border[o + 3] = 1.0f;
        border[o + 4] = 1.05f;
        border[o + 5] = opacity;
    }

    p_glBindBuffer(GL_ARRAY_BUFFER, g_vboFill);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(fill), fill, GL_DYNAMIC_DRAW);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vboBorder);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(border), border, GL_DYNAMIC_DRAW);
}

bool initGl() {
    if (g_ready) return true;
    if (g_failed) return false;
    if (!loadGl()) {
        logLine("PhaseHud: GLES procs missing");
        g_failed = true;
        return false;
    }

    GLuint vs = 0, fs = 0;
    if (!compileShader(GL_VERTEX_SHADER, kVS, vs)) {
        logLine("PhaseHud: VS compile fail");
        g_failed = true;
        return false;
    }
    if (!compileShader(GL_FRAGMENT_SHADER, kFS, fs)) {
        logLine("PhaseHud: FS compile fail");
        g_failed = true;
        return false;
    }

    g_prog = p_glCreateProgram();
    p_glAttachShader(g_prog, vs);
    p_glAttachShader(g_prog, fs);
    p_glLinkProgram(g_prog);
    GLint linked = 0;
    p_glGetProgramiv(g_prog, GL_LINK_STATUS, &linked);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    if (!linked) {
        if (p_glGetProgramInfoLog) {
            char log[256];
            p_glGetProgramInfoLog(g_prog, 255, nullptr, log);
            log[255] = 0;
            logLine("PhaseHud: link log: %s", log);
        }
        logLine("PhaseHud: link fail");
        g_failed = true;
        return false;
    }

    g_aPos = p_glGetAttribLocation(g_prog, "aPosition");
    g_aCol = p_glGetAttribLocation(g_prog, "aColor");

    p_glGenBuffers(1, &g_vboFill);
    p_glGenBuffers(1, &g_vboBorder);

    g_ready = true;
    logLine("PhaseHud: GL ready (simple 2D card)");
    return true;
}

void drawHud() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_mu);
    if (!initGl()) return;

    const float scale = g_scale.load(std::memory_order_relaxed);
    const float x = g_x.load(std::memory_order_relaxed);
    const float y = g_y.load(std::memory_order_relaxed);
    const float op = g_opacity.load(std::memory_order_relaxed);
    buildGeometry(scale, x, y, op);

    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_CULL_FACE);
    p_glDisable(GL_SCISSOR_TEST);
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    p_glUseProgram(g_prog);

    // FILL
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vboFill);
    if (g_aPos >= 0) {
        p_glEnableVertexAttribArray(static_cast<GLuint>(g_aPos));
        p_glVertexAttribPointer(static_cast<GLuint>(g_aPos), 2, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat),
                                reinterpret_cast<void*>(0));
    }
    if (g_aCol >= 0) {
        p_glEnableVertexAttribArray(static_cast<GLuint>(g_aCol));
        p_glVertexAttribPointer(static_cast<GLuint>(g_aCol), 4, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat),
                                reinterpret_cast<void*>(2 * sizeof(GLfloat)));
    }
    p_glDrawArrays(GL_TRIANGLE_FAN, 0, 7);

    // BORDER
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vboBorder);
    if (g_aPos >= 0) {
        p_glVertexAttribPointer(static_cast<GLuint>(g_aPos), 2, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat),
                                reinterpret_cast<void*>(0));
    }
    if (g_aCol >= 0) {
        p_glVertexAttribPointer(static_cast<GLuint>(g_aCol), 4, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat),
                                reinterpret_cast<void*>(2 * sizeof(GLfloat)));
    }
    if (p_glLineWidth) p_glLineWidth(3.f);
    p_glDrawArrays(GL_LINE_LOOP, 0, 5);

    if (g_drawLog < 5) {
        logLine("PhaseHud: draw #%d scale=%.2f x=%.2f y=%.2f", g_drawLog, scale, x, y);
        ++g_drawLog;
    }
}

EGLBoolean swapDetour(EGLDisplay dpy, EGLSurface surf) {
    drawHud();
    if (g_swapOriginal) return g_swapOriginal(dpy, surf);
    return EGL_FALSE;
}

void tryHook() {
    // disabled — MotionBlur owns eglSwapBuffers
}

void onToggle(std::string_view, bool en) {
    g_enabled.store(en, std::memory_order_release);
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "scale")
            g_scale.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "x")
            g_x.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "y")
            g_y.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "opacity")
            g_opacity.store(std::stof(std::string(value)), std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Phase HUD");
    b.description("2D Phase-style navy pentagon + white border (bottom). Pure visual overlay.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("scale", "Card size", pl::modmenu::ConfigType::SliderFloat, "0.18", "0.08", "0.40", "");
    b.config("x", "Horizontal (hand side)", pl::modmenu::ConfigType::SliderFloat, "0.62", "0.20", "0.90", "");
    b.config("y", "Vertical (NDC)", pl::modmenu::ConfigType::SliderFloat, "-0.72", "-0.95", "-0.20", "");
    b.config("opacity", "Opacity", pl::modmenu::ConfigType::SliderFloat, "0.95", "0.3", "1.0", "");
    b.registerModule();
}

void onSignaturesReady() {
    logLine("PhaseHud: ready (draw via MotionBlur onFrame only)");
}

void onFrame() {
    drawHud();
}

void shutdown() { g_enabled.store(false, std::memory_order_release); }

} // namespace bactro::phasehud
