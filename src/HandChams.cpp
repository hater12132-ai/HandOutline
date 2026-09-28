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

#define HC_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::handchams {
namespace {

struct Color {
    float r, g, b, a;
};

constexpr const char* kModuleId = "bactro.handchams";

std::atomic_bool g_enabled{false};
std::atomic_bool g_handOnly{false};
std::atomic_bool g_outlinePass{true}; // multi-pass inverted-cull outline
std::atomic_bool g_throughWalls{false};
std::atomic<float> g_r{0.88f};
std::atomic<float> g_g{0.95f};
std::atomic<float> g_b{1.00f};
std::atomic<float> g_opacity{0.55f};
std::atomic<float> g_intensity{1.35f};
std::atomic_int g_hits{0};
std::atomic_int g_meshHits{0};

Color g_chams{0.88f, 0.95f, 1.00f, 0.55f};
Color g_outline{1.00f, 1.00f, 1.00f, 1.00f};

bool g_renderFpHooked = false;
std::atomic_bool g_meshChamsArmed{false}; // setEntityConstants saw a chams-eligible draw
std::atomic_int g_pass{0}; // 0=normal, 1=outline, 2=fill

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

void tryInstallHooks() {
    void* o = nullptr;
    resolveGl();

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
        refresh();
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Snow Chams ESP");
    b.description(
         "Multi-pass snowy chams + white inverted-cull outline on hand/entities. "
         "Outline needs MeshHelpers hook. F5 to see own body. Join world, then enable.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("r", "Red", pl::modmenu::ConfigType::SliderFloat, "0.88", "0", "1", "");
    b.config("g", "Green", pl::modmenu::ConfigType::SliderFloat, "0.95", "0", "1", "");
    b.config("b", "Blue", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("opacity", "Opacity", pl::modmenu::ConfigType::SliderFloat, "0.55", "0.05", "1.0", "");
    b.config("intensity", "Intensity", pl::modmenu::ConfigType::SliderFloat, "1.35", "0.1", "2.5", "");
    b.config("outlinePass", "Multi-pass outline", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("throughWalls", "Outline through walls", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.config("handOnly", "Hand/items only", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.registerModule();
}

void onSignaturesReady() {
    logLine("SnowChams: ready — join world, then enable");
}

void shutdown() { g_enabled.store(false, std::memory_order_release); }

void onPostFrame() {}

} // namespace bactro::handchams
