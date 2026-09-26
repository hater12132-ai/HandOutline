#include "bactro/WireOutline.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <android/log.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#define WO_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::wireoutline {
namespace {

constexpr const char* kModuleId = "bactro.wireoutline";

std::atomic_bool g_enabled{false};
std::atomic_bool g_players{true};
std::atomic_bool g_crystals{true};
std::atomic<float> g_lineWidth{2.0f};

// Resolved from 1.26.51 signatures (addresses logged for RE progress)
using ActorIsPlayerFn = bool (*)(void*);
using HitResultGetEntityFn = void* (*)(void*); // HitResult* -> Actor*
ActorIsPlayerFn g_isPlayer = nullptr;
HitResultGetEntityFn g_hitGetEntity = nullptr;
std::uintptr_t g_levelGetHitResult = 0;
std::uintptr_t g_fetchNearby = 0;
std::uintptr_t g_clientGetLocalPlayer = 0;

void logLine(const char* fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    WO_LOGI("%s", buf);
}

void resolveSymbols() {
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
            logLine("WireOutline: FetchNearbyActors @%p", reinterpret_cast<void*>(g_fetchNearby));
    }
    if (!g_clientGetLocalPlayer) {
        g_clientGetLocalPlayer =
            bactro::memory::resolve(bactro::memory::SignatureId::ClientInstanceGetLocalPlayer);
        if (g_clientGetLocalPlayer)
            logLine("WireOutline: ClientInstanceGetLocalPlayer @%p",
                    reinterpret_cast<void*>(g_clientGetLocalPlayer));
    }

    // Documented layout from public MCBE SDKs + this 1.26.51 binary (ECS):
    // AABBShapeComponent: Vec3 mMin, Vec3 mMax, float mWidth, float mHeight  (~0x20)
    // StateVectorComponent: Vec3 mPos, mPosPrev, mPosDelta  (~0x24)
    // Camera: CameraAPIComponent / CameraClientInstanceComponent
    logLine("WireOutline: RE note AABBShape={min,max,w,h} StateVector={pos,prev,delta}");
    logLine("WireOutline: 3D wire needs camera matrix + component get (next)");
    logLine("WireOutline: resolved isPlayer=%d hitEnt=%d levelHit=%d nearby=%d localP=%d",
            g_isPlayer ? 1 : 0, g_hitGetEntity ? 1 : 0, g_levelGetHitResult ? 1 : 0,
            g_fetchNearby ? 1 : 0, g_clientGetLocalPlayer ? 1 : 0);
}

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    logLine("WireOutline %s", enabled ? "ON (resolving — no unsafe draw yet)" : "OFF");
    if (enabled) resolveSymbols();
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "players")
            g_players.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "crystals")
            g_crystals.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "lineWidth")
            g_lineWidth.store(std::stof(std::string(value)), std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Wire Outline");
    b.description(
         "White entity wire frame (crystal-style). Uses 1.26.51 RE: "
         "AABBShapeComponent + StateVector + CameraAPI. "
         "Currently resolves symbols only — 3D draw comes next once matrix is locked. "
         "Does NOT draw a 2D HUD card.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("players", "Players", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("crystals", "Crystals", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("lineWidth", "Line width", pl::modmenu::ConfigType::SliderFloat, "2.0", "1", "5", "");
    b.registerModule();
}

void onSignaturesReady() {
    logLine("WireOutline: ready — enable after join to resolve 1.26.51 symbols");
}

void shutdown() { g_enabled.store(false, std::memory_order_release); }

void onPostFrame() {
    // Intentionally empty until view-projection + AABB component access are verified.
    // Drawing without matrix caused the previous crash / useless corner box.
}

} // namespace bactro::wireoutline
