#include "SetlistActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <variant>

#include "SongActivity.h"
#include "SongbookFiles.h"

namespace fui = freeink::ui;

void SetlistActivity::onEnter() {
  SongbookListActivity::onEnter();
  emptyMessage = problem ? problem : tr(STR_SONGBOOK_SETLIST_EMPTY);
  rowItems.clear();
  if (problem) {
    requestUpdate();
    return;
  }

  const size_t count = setlist.songs.size();
  // Rows point into these strings, so both are sized before any is filled.
  labels.reserve(count);
  artists.reserve(count);
  rowItems.reserve(count);
  songbook::Song header;
  for (size_t i = 0; i < count; ++i) {
    const auto result = songbook::loadSong(setlist.songs[i], header, true);
    char number[8];
    snprintf(number, sizeof(number), "%u. ", static_cast<unsigned>(i + 1));
    labels.emplace_back(number);
    labels.back().append(header.meta.title.empty() ? setlist.songs[i] : header.meta.title);
    artists.emplace_back(header.meta.artist);

    fui::ListItem item;
    item.label = labels.back().c_str();
    if (!artists.back().empty()) item.subtitle = artists.back().c_str();
    switch (result) {
      case songbook::SongLoadResult::Ok:
        break;
      case songbook::SongLoadResult::TooLarge:
        item.value = tr(STR_SONGBOOK_FILE_TOO_LARGE);
        break;
      case songbook::SongLoadResult::Missing:
      case songbook::SongLoadResult::Empty:
      case songbook::SongLoadResult::OutOfMemory:
        item.value = tr(STR_SONGBOOK_FILE_MISSING);
        break;
    }
    item.actionValue = static_cast<int16_t>(i);
    rowItems.push_back(item);
  }
  requestUpdate();
}

void SetlistActivity::onExit() {
  SongbookListActivity::onExit();
  rowItems.clear();
  labels.clear();
  artists.clear();
}

void SetlistActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(rowItems.size())) return;
  app.clearTapFlash();
  nav.selected = index;
  auto activity = makeUniqueNoThrow<SongActivity>(renderer, mappedInput, setlist, static_cast<size_t>(index));
  if (!activity) {
    LOG_ERR("SONG", "OOM: song activity");
    return;
  }
  // The song screen reports the song it ended on so the list follows it.
  startActivityForResult(std::move(activity), [this](const ActivityResult& result) {
    if (const auto* page = std::get_if<PageResult>(&result.data)) {
      if (page->page < rowItems.size()) moveSelectionTo(static_cast<int>(page->page));
    }
  });
}
