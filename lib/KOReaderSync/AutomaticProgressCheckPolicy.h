#pragma once

#include <cmath>
#include <cstdint>

enum class AutomaticProgressDecision : uint8_t {
  IGNORE,
  PROMPT,
  INVALID_REMOTE,
};

class AutomaticProgressCheckPolicy {
 public:
  static constexpr float REMOTE_AHEAD_EPSILON = 0.001f;  // 0.1 percentage points

  static AutomaticProgressDecision decide(const float localPercentage, const float remotePercentage) {
    if (!std::isfinite(remotePercentage) || remotePercentage < 0.0f || remotePercentage > 1.0f) {
      return AutomaticProgressDecision::INVALID_REMOTE;
    }

    return remotePercentage - localPercentage > REMOTE_AHEAD_EPSILON ? AutomaticProgressDecision::PROMPT
                                                                     : AutomaticProgressDecision::IGNORE;
  }
};
