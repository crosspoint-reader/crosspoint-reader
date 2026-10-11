#include "HomeMenu.h"

#include "CrossPointSettings.h"

namespace {

// What an entry needs before it appears anywhere.
enum class Requires : uint8_t { Nothing, Bookmarks, WeatherEnabled };

struct HomeMenuRow {
  HomeMenuEntry entry;
  Requires requirement;
  // The "show on home screen" setting; nullptr pins the entry to the home screen.
  uint8_t CrossPointSettings::* shownOnHome;
};

// Display order, top to bottom (left to right on the carousel's icon row).
//
// Reading Stats requires nothing on purpose. Gating it on there being history would mean asking
// the stats store, which is deliberately not resident -- loaded on demand and released to give
// the heap back -- and the menu is rebuilt both inside and outside that window. Outside it every
// accessor reads zero, so the row would come and go depending on which rebuild ran last. The
// screen already says so plainly when there is nothing to show.
constexpr HomeMenuRow kRows[] = {
    {{HomeMenuAction::Library, StrId::STR_LIBRARY, Folder}, Requires::Nothing, &CrossPointSettings::showLibraryOnHome},
    {{HomeMenuAction::ReadingStats, StrId::STR_READING_STATS, Stats},
     Requires::Nothing,
     &CrossPointSettings::showReadingStatsOnHome},
    {{HomeMenuAction::GlobalBookmarks, StrId::STR_GLOBAL_BOOKMARKS, Book},
     Requires::Bookmarks,
     &CrossPointSettings::showBookmarksOnHome},
    {{HomeMenuAction::FileTransfer, StrId::STR_FILE_TRANSFER, Transfer},
     Requires::Nothing,
     &CrossPointSettings::showFileTransferOnHome},
    {{HomeMenuAction::Weather, StrId::STR_WEATHER, Weather},
     Requires::WeatherEnabled,
     &CrossPointSettings::showWeatherOnHome},
    {{HomeMenuAction::Settings, StrId::STR_SETTINGS_TITLE, Settings}, Requires::Nothing, nullptr},
};

constexpr HomeMenuEntry kMoreEntry{HomeMenuAction::More, StrId::STR_MORE, Ellipsis};

bool isAvailable(const Requires requirement, const HomeMenuAvailability& availability) {
  switch (requirement) {
    case Requires::Nothing:
      return true;
    case Requires::Bookmarks:
      return availability.hasBookmarks;
    case Requires::WeatherEnabled:
      return SETTINGS.useWeather != 0;
  }
  return false;
}

HomeMenuPlacement placementOf(const HomeMenuRow& row) {
  if (row.shownOnHome == nullptr || SETTINGS.*row.shownOnHome) return HomeMenuPlacement::Home;
  return HomeMenuPlacement::More;
}

}  // namespace

void collectHomeMenuEntries(const HomeMenuPlacement placement, const HomeMenuAvailability& availability,
                            std::vector<HomeMenuEntry>& out) {
  bool anyMovedToMore = false;
  for (const HomeMenuRow& row : kRows) {
    if (isAvailable(row.requirement, availability) && placementOf(row) == HomeMenuPlacement::More) {
      anyMovedToMore = true;
      break;
    }
  }

  for (const HomeMenuRow& row : kRows) {
    if (!isAvailable(row.requirement, availability)) continue;
    // More goes directly above the pinned entry (Settings), the one that can never move to it.
    if (placement == HomeMenuPlacement::Home && anyMovedToMore && row.shownOnHome == nullptr) {
      out.push_back(kMoreEntry);
    }
    if (placementOf(row) == placement) out.push_back(row.entry);
  }
}

uint8_t libraryOnHomeFromLegacy(const uint8_t browseFiles, const uint8_t recentBooks) {
  return browseFiles != 0 || recentBooks != 0 ? 1 : 0;
}
