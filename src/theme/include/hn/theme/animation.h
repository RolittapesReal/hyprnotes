#pragma once
#include <algorithm>

namespace hn::theme {
// Interaction animations run 100-150 ms, or not at all under reduced motion.
struct AnimationPolicy {
    bool reduceMotion = false;
    int duration(int requestedMs = 120) const { return reduceMotion ? 0 : std::clamp(requestedMs, 100, 150); }
};
} // namespace hn::theme
