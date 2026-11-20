#pragma once

#include <cstring>
#include <string_view>

// Only unsaved positions override the card. RTC_DATA_ATTR storage keeps this
// state through deep sleep; a cold boot starts with the on-card cursor again.
struct SleepImageCursor {
  static constexpr size_t PATH_CAPACITY = 288;

  const char* lastShown(const char* savedPath) const { return pending ? path : savedPath; }

  bool record(const std::string_view shownPath, const bool saved) {
    pending = false;
    if (saved) return true;
    if (shownPath.empty() || shownPath.size() >= sizeof(path)) return false;
    std::memcpy(path, shownPath.data(), shownPath.size());
    path[shownPath.size()] = '\0';
    pending = true;
    return true;
  }

 private:
  char path[PATH_CAPACITY] = {};
  bool pending = false;
};
