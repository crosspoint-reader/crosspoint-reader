#pragma once

#include <cmath>
#include <cstdint>

enum class AutomaticProgressUploadDecision : uint8_t {
  SKIP,
  UPLOAD,
  INVALID_PROGRESS,
};

class AutomaticProgressUploadPolicy {
 public:
  static constexpr float LOCAL_AHEAD_EPSILON = 0.001f;  // 0.1 percentage points

  static AutomaticProgressUploadDecision decide(const float localPercentage, const bool hasRemoteProgress,
                                                const float remotePercentage) {
    if (!validPercentage(localPercentage) || (hasRemoteProgress && !validPercentage(remotePercentage))) {
      return AutomaticProgressUploadDecision::INVALID_PROGRESS;
    }

    if (!hasRemoteProgress) {
      return AutomaticProgressUploadDecision::UPLOAD;
    }

    return localPercentage - remotePercentage > LOCAL_AHEAD_EPSILON ? AutomaticProgressUploadDecision::UPLOAD
                                                                    : AutomaticProgressUploadDecision::SKIP;
  }

 private:
  static bool validPercentage(const float percentage) {
    return std::isfinite(percentage) && percentage >= 0.0f && percentage <= 1.0f;
  }
};
