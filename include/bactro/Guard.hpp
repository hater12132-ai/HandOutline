#pragma once
// Crash-loop guard. If the game dies within the first seconds after the actor hooks start running,
// the next launch starts in SAFE MODE (hooks skipped) so you can still get into your world.
// States in bactro_guard.txt:  testing = hooks ran, not yet proven stable | ok = stable | safe = last start was safe.
#include <atomic>
#include <fstream>
#include <string>

namespace bactro::guard {

inline const char* const kPaths[2] = {
    "/storage/emulated/0/Android/media/org.levimc.launcher/bactro_guard.txt",
    "/sdcard/Android/media/org.levimc.launcher/bactro_guard.txt",
};

inline std::atomic_bool testing{false};
inline std::atomic_int frames{0};

inline std::string readState() {
    for (const char* p : kPaths) {
        std::ifstream in(p);
        if (!in) continue;
        std::string s;
        std::getline(in, s);
        return s;
    }
    return {};
}

inline void writeState(const char* s) {
    for (const char* p : kPaths) {
        std::ofstream out(p, std::ios::trunc);
        if (!out) continue;
        out << s << '\n';
        return;
    }
}

// Call once at load. true = previous session crashed during probation -> run in safe mode now.
inline bool beginSession() {
    if (readState() == "testing") {
        writeState("safe");
        return true;
    }
    return false;
}

// Call from the first risky hook that actually runs (once per session).
inline void noteRiskyHookRan() {
    bool expected = false;
    if (testing.compare_exchange_strong(expected, true)) writeState("testing");
}

// Call once per rendered frame. ~600 frames after the hooks started = considered stable.
inline void frameTick() {
    if (!testing.load(std::memory_order_relaxed)) return;
    if (frames.fetch_add(1, std::memory_order_relaxed) == 600) writeState("ok");
}

} // namespace bactro::guard
