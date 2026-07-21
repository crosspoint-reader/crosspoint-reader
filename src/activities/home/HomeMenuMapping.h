#pragma once

#include <cstdint>

enum class HomeMenuItem : uint8_t {
  NONE,
  FILE_BROWSER,
  RECENTS,
  SAVED_ITEMS,
  OPDS_BROWSER,
  FILE_TRANSFER,
  SETTINGS_MENU,
  READING_STATS,
};

namespace HomeMenuMapping {

constexpr int itemCount(const bool hasOpds, const bool hasReadingStats) {
  return 5 + (hasOpds ? 1 : 0) + (hasReadingStats ? 1 : 0);
}

constexpr int selectionCount(const int recentBookCount, const bool hasOpds, const bool hasReadingStats) {
  return (recentBookCount > 0 ? recentBookCount : 0) + itemCount(hasOpds, hasReadingStats);
}

constexpr int indexOf(const HomeMenuItem action, const bool hasOpds, const bool hasReadingStats) {
  switch (action) {
    case HomeMenuItem::FILE_BROWSER:
      return 0;
    case HomeMenuItem::RECENTS:
      return 1;
    case HomeMenuItem::SAVED_ITEMS:
      return 2;
    case HomeMenuItem::OPDS_BROWSER:
      return hasOpds ? 3 : -1;
    case HomeMenuItem::FILE_TRANSFER:
      return 3 + (hasOpds ? 1 : 0);
    case HomeMenuItem::SETTINGS_MENU:
      return 4 + (hasOpds ? 1 : 0);
    case HomeMenuItem::READING_STATS:
      return hasReadingStats ? 5 + (hasOpds ? 1 : 0) : -1;
    case HomeMenuItem::NONE:
    default:
      return -1;
  }
}

constexpr HomeMenuItem actionAt(const int index, const bool hasOpds, const bool hasReadingStats) {
  if (index < 0 || index >= itemCount(hasOpds, hasReadingStats)) return HomeMenuItem::NONE;
  if (index == indexOf(HomeMenuItem::FILE_BROWSER, hasOpds, hasReadingStats)) return HomeMenuItem::FILE_BROWSER;
  if (index == indexOf(HomeMenuItem::RECENTS, hasOpds, hasReadingStats)) return HomeMenuItem::RECENTS;
  if (index == indexOf(HomeMenuItem::SAVED_ITEMS, hasOpds, hasReadingStats)) return HomeMenuItem::SAVED_ITEMS;
  if (index == indexOf(HomeMenuItem::OPDS_BROWSER, hasOpds, hasReadingStats)) return HomeMenuItem::OPDS_BROWSER;
  if (index == indexOf(HomeMenuItem::FILE_TRANSFER, hasOpds, hasReadingStats)) return HomeMenuItem::FILE_TRANSFER;
  if (index == indexOf(HomeMenuItem::SETTINGS_MENU, hasOpds, hasReadingStats)) return HomeMenuItem::SETTINGS_MENU;
  if (index == indexOf(HomeMenuItem::READING_STATS, hasOpds, hasReadingStats)) return HomeMenuItem::READING_STATS;
  return HomeMenuItem::NONE;
}

constexpr int selectorIndexOf(const HomeMenuItem action, const int recentBookCount, const bool hasOpds,
                              const bool hasReadingStats) {
  const int menuIndex = indexOf(action, hasOpds, hasReadingStats);
  return menuIndex < 0 ? -1 : (recentBookCount > 0 ? recentBookCount : 0) + menuIndex;
}

}  // namespace HomeMenuMapping
