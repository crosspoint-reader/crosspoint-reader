#include "SongbookActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <utility>

#include "SetlistActivity.h"

namespace fui = freeink::ui;

const char* SongbookActivity::headerTitle() const { return tr(STR_SONGBOOK); }

void SongbookActivity::onEnter() {
  SongbookListActivity::onEnter();
  rowItems.clear();
  if (!songbook::songbookFolderExists()) {
    emptyMessage = tr(STR_SONGBOOK_NO_FOLDER);
    requestUpdate();
    return;
  }
  songbook::listSetlists(setlists);
  hasSongs = songbook::hasSongFiles();

  rowItems.reserve(setlists.size() + 1);
  for (const auto& setlist : setlists) {
    fui::ListItem item;
    item.label = setlist.title.c_str();
    switch (setlist.status) {
      case songbook::SetlistStatus::Ok:
        break;
      case songbook::SetlistStatus::Unreadable:
        item.value = tr(STR_SONGBOOK_SETLIST_INVALID);
        break;
      case songbook::SetlistStatus::NoSongs:
        item.value = tr(STR_SONGBOOK_SETLIST_EMPTY);
        break;
    }
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
  if (hasSongs) {
    fui::ListItem item;
    item.label = tr(STR_SONGBOOK_ALL_SONGS);
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
  emptyMessage = tr(STR_SONGBOOK_NOTHING_FOUND);
  requestUpdate();
}

void SongbookActivity::onExit() {
  SongbookListActivity::onExit();
  rowItems.clear();
  setlists.clear();
}

void SongbookActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(rowItems.size())) return;
  app.clearTapFlash();
  nav.selected = index;

  songbook::Setlist setlist;
  const char* problem = nullptr;
  if (index < static_cast<int>(setlists.size())) {
    songbook::SetlistParseResult result = songbook::SetlistParseResult::InvalidJson;
    if (!songbook::loadSetlist(setlists[index].file, setlist, result) ||
        result == songbook::SetlistParseResult::InvalidJson) {
      problem = tr(STR_SONGBOOK_SETLIST_INVALID);
    } else if (result == songbook::SetlistParseResult::NoSongs) {
      problem = tr(STR_SONGBOOK_SETLIST_EMPTY);
    }
    if (setlist.title.empty()) setlist.title = setlists[index].title;
  } else {
    songbook::listSongFiles(setlist.songs);
    setlist.title = tr(STR_SONGBOOK_ALL_SONGS);
  }

  auto activity = makeUniqueNoThrow<SetlistActivity>(renderer, mappedInput, std::move(setlist), problem);
  if (!activity) {
    LOG_ERR("SONG", "OOM: setlist activity");
    return;
  }
  activityManager.pushActivity(std::move(activity));
}
