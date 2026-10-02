#include "bactro/EntityOutline.hpp"
#include "bactro/RenderPhase.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <android/log.h>
#include <dlfcn.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#define EO_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::entityoutline {
namespace {

constexpr const char* kModuleId = "bactro.entityoutline";

std::atomic_bool g_enabled{false};
std::atomic_bool g_playersOnly{false}; // false = all living actors
std::atomic<float> g_width{3.0f};
std::atomic<float> g_glow{0.7f};
std::atomic<float> g_r{1.f}, g_g{1.f}, g_b{1.f}, g_a{1.f};

using ActorIsPlayerFn = bool (*)(void*);
ActorIsPlayerFn g_isPlayer = nullptr;

struct Color {
    float r, g, b, a;
};

using SetupActorGlintFn = void (*)(void*, void*, void*, const Color*, const Color*, const Color*, const Color*,
                                   float, float, float, float, const void*);
SetupActorGlintFn g_setupActorGlint = nullptr;
bool g_glintHooked = false;

using PFN_glDrawElements = void (*)(unsigned int, int, unsigned int, const void*);
using PFN_glLineWidth = void (*)(float);
using PFN_glDepthMask = void (*)(unsigned char);
using PFN_glEnable = void (*)(unsigned int);
using PFN_glDisable = void (*)(unsigned int);
using PFN_glBlendFunc = void (*)(unsigned int, unsigned int);

PFN_glDrawElements g_drawElementsOrig = nullptr;
PFN_glLineWidth g_glLineWidth = nullptr;
PFN_glDepthMask g_glDepthMask = nullptr;
PFN_glEnable g_glEnable = nullptr;
PFN_glDisable g_glDisable = nullptr;
PFN_glBlendFunc g_glBlendFunc = nullptr;
bool g_drawHooked = false;
std::atomic_int g_wireCount{0};

void logLine(const char* fmt, ...) {
    char buf[240];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    EO_LOGI("%s", buf);
}

void* glProc(const char* name) {
    void* p = nullptr;
    void* gles = dlopen("libGLESv2.so", RTLD_NOLOAD);
    if (!gles) gles = dlopen("libGLESv3.so", RTLD_NOLOAD);
    if (!gles) gles = dlopen("libGLESv2.so", RTLD_NOW);
    if (gles) p = dlsym(gles, name);
    if (!p) {
        void* egl = dlopen("libEGL.so", RTLD_NOW);
        if (egl) {
            using GPA = void* (*)(const char*);
            auto gpa = reinterpret_cast<GPA>(dlsym(egl, "eglGetProcAddress"));
            if (gpa) p = gpa(name);
        }
    }
    return p;
}

void resolveGl() {
    if (!g_glLineWidth) g_glLineWidth = reinterpret_cast<PFN_glLineWidth>(glProc("glLineWidth"));
    if (!g_glDepthMask) g_glDepthMask = reinterpret_cast<PFN_glDepthMask>(glProc("glDepthMask"));
    if (!g_glEnable) g_glEnable = reinterpret_cast<PFN_glEnable>(glProc("glEnable"));
    if (!g_glDisable) g_glDisable = reinterpret_cast<PFN_glDisable>(glProc("glDisable"));
    if (!g_glBlendFunc) g_glBlendFunc = reinterpret_cast<PFN_glBlendFunc>(glProc("glBlendFunc"));
}

void setupActorGlintDetour(void* screenContext, void* entityContext, void* actor, const Color* overlay,
                           const Color* changeColor, const Color* changeColor2, const Color* glintColor,
                           float uvOffset1, float uvOffset2, float uvRot1, float uvRot2,
                           const void* lightEmissionColor) {
    if (g_enabled.load(std::memory_order_relaxed) && actor) {
        bool arm = true;
        if (g_playersOnly.load(std::memory_order_relaxed) && g_isPlayer) {
            try {
                arm = g_isPlayer(actor);
            } catch (...) {
                arm = true;
            }
        }
        if (arm)
            bactro::phase::entityMeshArmed.store(true, std::memory_order_release);
    }
    if (g_setupActorGlint)
        g_setupActorGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor,
                          uvOffset1, uvOffset2, uvRot1, uvRot2, lightEmissionColor);
}

void glDrawElementsDetour(unsigned int mode, int count, unsigned int type, const void* indices) {
    if (g_drawElementsOrig)
        g_drawElementsOrig(mode, count, type, indices);

    if (!g_enabled.load(std::memory_order_relaxed))
        return;
    if (!bactro::phase::entityMeshArmed.load(std::memory_order_acquire))
        return;
    // GL_TRIANGLES
    if (mode != 0x0004u) return;
    if (count < 6 || count > 200000) return;
    if (!g_drawElementsOrig) return;

    resolveGl();
    const float width = std::max(0.5f, std::min(12.f, g_width.load(std::memory_order_relaxed)));
    const float glow = std::max(0.f, std::min(1.f, g_glow.load(std::memory_order_relaxed)));

    if (g_glEnable) g_glEnable(0x0BE2); // GL_BLEND
    if (g_glBlendFunc) g_glBlendFunc(0x0302, 0x0303);
    if (g_glDepthMask) g_glDepthMask(0);

    // Glow halo: wider line passes first
    if (glow > 0.05f && g_glLineWidth) {
        const int passes = 1 + static_cast<int>(glow * 3.f);
        for (int i = passes; i >= 1; --i) {
            g_glLineWidth(width * (1.f + 0.6f * static_cast<float>(i)));
            g_drawElementsOrig(0x0001u /*GL_LINES*/, count, type, indices);
        }
    }

    if (g_glLineWidth) g_glLineWidth(width);
    g_drawElementsOrig(0x0001u /*GL_LINES*/, count, type, indices);

    if (g_glDepthMask) g_glDepthMask(1);
    g_wireCount.fetch_add(1, std::memory_order_relaxed);
    // clear after use so only this mesh is outlined
    bactro::phase::entityMeshArmed.store(false, std::memory_order_release);
}

bool installGlintArm() {
    if (g_glintHooked) return true;
    if (!g_isPlayer) {
        const auto addr = bactro::memory::resolve(bactro::memory::SignatureId::ActorIsPlayer);
        if (addr) g_isPlayer = reinterpret_cast<ActorIsPlayerFn>(addr);
    }
    void* o = nullptr;
    if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetupShaderParametersActorGlint,
                             reinterpret_cast<void*>(&setupActorGlintDetour), &o) &&
        o) {
        g_setupActorGlint = reinterpret_cast<SetupActorGlintFn>(o);
        g_glintHooked = true;
        logLine("EntityOutline: setupActorGlint ARMED (entity filter)");
        return true;
    }
    logLine("EntityOutline: setupActorGlint FAIL (HandChams may own it — shared phase arm OK)");
    return false;
}

bool installDrawHook() {
    if (g_drawHooked) return true;
    void* p = glProc("glDrawElements");
    if (!p) {
        logLine("EntityOutline: glDrawElements not found");
        return false;
    }
    void* o = nullptr;
    if (pl::memory::hook(p, reinterpret_cast<void*>(&glDrawElementsDetour), &o) != 0 || !o) {
        logLine("EntityOutline: glDrawElements HOOK FAIL");
        return false;
    }
    g_drawElementsOrig = reinterpret_cast<PFN_glDrawElements>(o);
    resolveGl();
    g_drawHooked = true;
    logLine("EntityOutline: glDrawElements HOOKED width=%.1f glow=%.2f", g_width.load(), g_glow.load());
    return true;
}

void tryInstall() {
    installGlintArm();
    installDrawHook();
}

void onToggle(std::string_view, bool on) {
    g_enabled.store(on, std::memory_order_release);
    logLine(on ? "EntityOutline ON" : "EntityOutline OFF");
    if (on) tryInstall();
    else bactro::phase::entityMeshArmed.store(false, std::memory_order_release);
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "width")
            g_width.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "glow")
            g_glow.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "r")
            g_r.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "g")
            g_g.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "b")
            g_b.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "alpha")
            g_a.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "playersOnly")
            g_playersOnly.store(value == "true" || value == "1", std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Entity Outline");
    b.description("GLES mesh edge outline on players/mobs (width + glow). Separate from Hand Chams.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("width", "Outline width", pl::modmenu::ConfigType::SliderFloat, "3.0", "0.5", "12", "");
    b.config("glow", "Outline glow", pl::modmenu::ConfigType::SliderFloat, "0.7", "0", "1", "");
    b.config("r", "Color R", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("g", "Color G", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("b", "Color B", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("alpha", "Alpha", pl::modmenu::ConfigType::SliderFloat, "1.00", "0.1", "1", "");
    b.config("playersOnly", "Players only", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.registerModule();
}

void onSignaturesReady() {
    logLine("EntityOutline: ready — enable for mesh edge outline (width/glow)");
    if (g_enabled.load(std::memory_order_relaxed)) tryInstall();
}

void shutdown() {
    g_enabled.store(false, std::memory_order_release);
    bactro::phase::entityMeshArmed.store(false, std::memory_order_release);
}

} // namespace bactro::entityoutline
