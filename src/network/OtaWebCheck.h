#pragma once

#include <string>

// OTA web-triggered update-check service.
// Manages a single FreeRTOS background task that calls OtaUpdater::checkForUpdate()
// and stores the result in a mutex-guarded static. Thread-safe for concurrent
// POST (start) + GET (poll) from the web server task.

namespace network {

enum class OtaWebStartResult {
  Disabled,
  Started,
  AlreadyChecking,
  StartTaskFailed,
};

enum class OtaWebCheckStatus {
  Disabled,
  Idle,
  Checking,
  Done,
};

struct OtaWebCheckSnapshot {
  OtaWebCheckStatus status = OtaWebCheckStatus::Disabled;
  bool available = false;
  std::string latestVersion;
  std::string message;
  int errorCode = 0;
};

class OtaWebCheck {
 public:
  static OtaWebStartResult start();
  static OtaWebCheckSnapshot getSnapshot();
};

}  // namespace network
