#include "bactro/WireOutline.hpp"
#include "bactro/RenderPhase.hpp"
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

// Target categories — outline will apply to all enabled groups
std::atomic_bool g_handItems{true};   // FP hand, weapons, tools, blocks in hand
std::atomic_bool g_armor{true};       // worn / held armor pieces when rendered
std::atomic_bool g_players{true};     // other players
std::atomic_bool g_crystals{true};    // end crystals
std::atomic<float> g_lineWidth{2.0f};

using ActorIsPlayerFn = bool (*)(void*);
using HitResultGetEntityFn = void* (*)(void*);
ActorIsPlayerFn g_isPlayer = nullptr;
HitResultGetEntityFn g_hitGetEntity = nullptr;
std::uintptr_t g_levelGetHitResult = 0;
std::uintptr_t g_fetchNearby = 0;
std::uintptr_t g_clientGetLocalPlayer = 0;
bool g_fpHooked = false;

using RenderFirstPersonFn = void (*)(void*, void*, void*, void*, void*, void*);
RenderFirstPersonFn g_renderFpOriginal = nullptr;

void logLine(const char* fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    WO_LOGI("%s", buf);
}

// Track FP hand/item render window (same flag HandChams uses)
void renderFirstPersonDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!g_renderFpOriginal) return;
    const bool wantHand = g_enabled.load(std::memory_order_relaxed) &&
                          g_handItems.load(std::memory_order_relaxed);
    if (wantHand)
        bactro::phase::inFirstPersonHand.store(true, std::memory_order_release);
    g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    if (wantHand)
        bactro::phase::inFirstPersonHand.store(false, std::memory_order_release);
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
            logLine("WireOutline: GetLocalPlayer @%p", reinterpret_cast<void*>(g_clientGetLocalPlayer));
    }

    // FP hand / held item / weapon path (shares ItemInHandRenderer with HandChams)
    if (!g_fpHooked) {
        void* o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ItemInHandRendererRenderFirstPerson,
                                 reinterpret_cast<void*>(&renderFirstPersonDetour), &o) &&
            o) {
            g_renderFpOriginal = reinterpret_cast<RenderFirstPersonFn>(o);
            g_fpHooked = true;
            logLine("WireOutline: renderFirstPerson hooked (hand/items/weapons path)");
        } else {
            logLine("WireOutline: renderFirstPerson FAIL (hand path)");
        }
    }

    logLine("WireOutline: targets handItems=%d armor=%d players=%d crystals=%d",
            g_handItems.load() ? 1 : 0, g_armor.load() ? 1 : 0, g_players.load() ? 1 : 0,
            g_crystals.load() ? 1 : 0);
    logLine("WireOutline: resolved isPlayer=%d hitEnt=%d levelHit=%d nearby=%d localP=%d fp=%d",
            g_isPlayer ? 1 : 0, g_hitGetEntity ? 1 : 0, g_levelGetHitResult ? 1 : 0,
            g_fetchNearby ? 1 : 0, g_clientGetLocalPlayer ? 1 : 0, g_fpHooked ? 1 : 0);
    logLine("WireOutline: 3D draw still pending (camera + AABB) — no 2D card");
}

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    logLine("WireOutline %s", enabled ? "ON" : "OFF");
    if (enabled) resolveSymbols();
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
        else if (key == "lineWidth")
            g_lineWidth.store(std::stof(std::string(value)), std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Wire Outline");
    b.description(
         "White outline targets (when 3D draw is ready):\n"
         "• Hand / held items / weapons / tools\n"
         "• Armor\n"
         "• Players\n"
         "• End crystals\n"
         "No 2D HUD card. Symbol resolve works on 1.26.51; line draw next.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("handItems", "Hand / items / weapons", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("armor", "Armor", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("players", "Players", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("crystals", "Crystals", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("lineWidth", "Line width", pl::modmenu::ConfigType::SliderFloat, "2.0", "1", "5", "");
    b.registerModule();
}

void onSignaturesReady() {
    logLine("WireOutline: ready — enable after join");
}

void shutdown() { g_enabled.store(false, std::memory_order_release); }

void onPostFrame() {
    // No draw until camera matrix + AABB are safe.
}

} // namespace bactro::wireoutline
