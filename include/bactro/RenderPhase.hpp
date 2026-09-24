#pragma once
#include <atomic>

namespace bactro::phase {
inline std::atomic_bool inFirstPersonHand{false};
inline std::atomic_bool outlinePass{false};
} // namespace bactro::phase
