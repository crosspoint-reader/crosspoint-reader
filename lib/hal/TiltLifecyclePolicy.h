#pragma once

#include <cstdint>

namespace TiltLifecyclePolicy {

inline bool shouldBeAwake(const uint8_t mode, const bool visibleReader) { return mode != 0 && visibleReader; }

}  // namespace TiltLifecyclePolicy
