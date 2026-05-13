#include "GlobalBookmarksActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "../ActivityManager.h"
#include "BookmarkStore.h"
#include "CrossPointState.h"
#include "GlobalBookmarkIndex.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void GlobalBookmarksActivity::onEnter() {
  Activity::onEnter();

  GLOBAL_BOOKMARKS.reconcile();
  rebuildRows();

  const int first = firstSelectableIndex();
  selectorIndex = first >= 0 ? first : 0;

  if (restoreHint.target == ReturnTo::GlobalBookmarks) {
    const auto& entries = GLOBAL_BOOKMARKS.getEntries();
    if (!restoreHint.selectionContext.empty() && restoreHint.selectBookmarkIndex >= 0) {
      for (size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (row.isSeparator) continue;
        if (row.bookmarkIndex == static_cast<size_t>(restoreHint.selectBookmarkIndex) &&
            row.bookIndex < entries.size() && entries[row.bookIndex].sourcePath == restoreHint.selectionContext) {
          selectorIndex = static_cast<int>(i);
          break;
        }
      }
    }
    if (selectorIndex < 0 || selectorIndex >= static_cast<int>(rows.size()) || isSeparatorRow(selectorIndex)) {
      const int fallback = firstSelectableIndex();
      selectorIndex = fallback >= 0 ? fallback : 0;
    }
    restoreHint = {};
  }

  const auto total = static_cast<int>(rows.size());
  buttonNavigator.setSelectablePredicate([this](int index) { return !isSeparatorRow(index); }, total);

  requestUpdate();
}

void GlobalBookmarksActivity::onExit() {
  Activity::onExit();
  rows.clear();
  buttonNavigator.clearSelectablePredicate();
}

void GlobalBookmarksActivity::rebuildRows() {
  rows.clear();
  const auto& entries = GLOBAL_BOOKMARKS.getEntries();
  for (size_t bi = 0; bi < entries.size(); bi++) {
    const auto& entry = entries[bi];
    if (entry.bookmarks.empty()) continue;
    Row sep;
    sep.isSeparator = true;
    sep.bookIndex = bi;
    rows.push_back(sep);
    for (size_t mi = 0; mi < entry.bookmarks.size(); mi++) {
      Row row;
      row.isSeparator = false;
      row.bookIndex = bi;
      row.bookmarkIndex = mi;
      rows.push_back(row);
    }
  }
}

bool GlobalBookmarksActivity::isSeparatorRow(int index) const {
  return index >= 0 && index < static_cast<int>(rows.size()) && rows[index].isSeparator;
}

int GlobalBookmarksActivity::firstSelectableIndex() const {
  for (size_t i = 0; i < rows.size(); i++) {
    if (!rows[i].isSeparator) return static_cast<int>(i);
  }
  return -1;
}

std::string GlobalBookmarksActivity::getRowTitle(int index) const {
  if (index < 0 || index >= static_cast<int>(rows.size())) return {};
  const auto& row = rows[index];
  const auto& entries = GLOBAL_BOOKMARKS.getEntries();
  if (row.bookIndex >= entries.size()) return {};
  const auto& entry = entries[row.bookIndex];

  if (row.isSeparator) {
    return UITheme::makeSeparatorTitle(entry.title.empty() ? entry.sourcePath : entry.title);
  }

  if (row.bookmarkIndex >= entry.bookmarks.size()) return {};
  const auto& bm = entry.bookmarks[row.bookmarkIndex];
  if (!bm.name.empty()) return bm.name;

  char buf[64];
  if (entry.isTxt) {
    snprintf(buf, sizeof(buf), "%s%d", tr(STR_PAGE_PREFIX), bm.pageNumber + 1);
  } else {
    snprintf(buf, sizeof(buf), "%s%d, %s%d", tr(STR_SECTION_PREFIX), bm.spineIndex + 1, tr(STR_PAGE_PREFIX),
             bm.pageNumber + 1);
  }
  return std::string(buf);
}

void GlobalBookmarksActivity::openSelected() {
  if (isSeparatorRow(selectorIndex)) return;
  const auto& row = rows[selectorIndex];
  const auto& entries = GLOBAL_BOOKMARKS.getEntries();
  if (row.bookIndex >= entries.size()) return;
  const auto& entry = entries[row.bookIndex];
  if (row.bookmarkIndex >= entry.bookmarks.size()) return;
  const auto& bm = entry.bookmarks[row.bookmarkIndex];

  if (!Storage.exists(entry.sourcePath.c_str())) {
    LOG_ERR("GBA", "Source file missing, reconciling: %s", entry.sourcePath.c_str());
    GLOBAL_BOOKMARKS.removeBySourcePath(entry.sourcePath);
    GLOBAL_BOOKMARKS.save();
    rebuildRows();
    const int first = firstSelectableIndex();
    selectorIndex = first >= 0 ? first : 0;
    buttonNavigator.setSelectablePredicate([this](int index) { return !isSeparatorRow(index); },
                                           static_cast<int>(rows.size()));
    requestUpdate();
    return;
  }

  auto& jump = APP_STATE.pendingBookmarkJump;
  jump.active = true;
  jump.bookPath = entry.sourcePath;
  jump.spineIndex = bm.spineIndex;
  jump.pageNumber = bm.pageNumber;
  APP_STATE.saveToFile();

  LOG_DBG("GBA", "Jumping to bookmark in %s at %u/%u", entry.sourcePath.c_str(), bm.spineIndex, bm.pageNumber);
  ReturnHint hint;
  hint.target = ReturnTo::GlobalBookmarks;
  hint.selectionContext = entry.sourcePath;
  hint.selectBookmarkIndex = static_cast<int>(row.bookmarkIndex);
  activityManager.replaceWithReader(entry.sourcePath, std::move(hint));
}

template <typename Op>
void GlobalBookmarksActivity::mutateBook(size_t bookIndex, Op&& op) {
  const auto& entries = GLOBAL_BOOKMARKS.getEntries();
  if (bookIndex >= entries.size()) return;
  const auto entry = entries[bookIndex];  // copy — index may invalidate after sync

  BookmarkStore store;
  store.load(entry.cacheDir);
  if (!op(store)) return;
  store.save();

  GLOBAL_BOOKMARKS.syncFromStore(store, entry.sourcePath, entry.cacheDir, entry.title, entry.isTxt);
  GLOBAL_BOOKMARKS.save();
}

void GlobalBookmarksActivity::deleteSelected() {
  if (isSeparatorRow(selectorIndex)) return;
  const auto& row = rows[selectorIndex];
  const size_t bookmarkIndex = row.bookmarkIndex;

  mutateBook(row.bookIndex, [bookmarkIndex](BookmarkStore& store) {
    if (bookmarkIndex >= store.getAll().size()) return false;
    store.removeAt(bookmarkIndex);
    return true;
  });

  rebuildRows();
  const int total = static_cast<int>(rows.size());
  buttonNavigator.setSelectablePredicate([this](int index) { return !isSeparatorRow(index); }, total);

  if (rows.empty()) {
    onGoHome();
    return;
  }
  if (selectorIndex >= total) selectorIndex = total - 1;
  if (isSeparatorRow(selectorIndex)) {
    const int next = ButtonNavigator::nextIndex(selectorIndex, total, [this](int i) { return !isSeparatorRow(i); });
    if (next >= 0) selectorIndex = next;
  }
  requestUpdate();
}

void GlobalBookmarksActivity::renameSelected() {
  if (isSeparatorRow(selectorIndex)) return;
  const auto& row = rows[selectorIndex];
  const auto& entries = GLOBAL_BOOKMARKS.getEntries();
  if (row.bookIndex >= entries.size()) return;
  const auto& entry = entries[row.bookIndex];
  if (row.bookmarkIndex >= entry.bookmarks.size()) return;

  const size_t bookIndex = row.bookIndex;
  const size_t bookmarkIndex = row.bookmarkIndex;
  const std::string initial =
      entry.bookmarks[bookmarkIndex].name.empty() ? getRowTitle(selectorIndex) : entry.bookmarks[bookmarkIndex].name;

  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_RENAME), initial,
                                                                 BookmarkStore::MAX_NAME_LENGTH, InputType::Text),
                         [this, bookIndex, bookmarkIndex](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             const auto& kr = std::get<KeyboardResult>(result.data);
                             mutateBook(bookIndex, [bookmarkIndex, &kr](BookmarkStore& store) {
                               if (bookmarkIndex >= store.getAll().size()) return false;
                               store.rename(bookmarkIndex, kr.text);
                               return true;
                             });
                             rebuildRows();
                             buttonNavigator.setSelectablePredicate([this](int i) { return !isSeparatorRow(i); },
                                                                    static_cast<int>(rows.size()));
                           }
                           requestUpdate();
                         });
}

void GlobalBookmarksActivity::loop() {
  const int total = static_cast<int>(rows.size());

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  if (total == 0) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelected();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    renameSelected();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    deleteSelected();
    return;
  }

  // Navigator is restricted to Up/Down so the Left (rename) and Right (delete)
  // handlers above cannot race default cursor movement on the same press tick.
  buttonNavigator.onNextList({MappedInputManager::Button::Down}, selectorIndex, total, [this] { requestUpdate(); });
  buttonNavigator.onPreviousList({MappedInputManager::Button::Up}, selectorIndex, total, [this] { requestUpdate(); });
}

void GlobalBookmarksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_GLOBAL_BOOKMARKS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;

  if (rows.empty()) {
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, contentTop + 20,
                      tr(STR_NO_GLOBAL_BOOKMARKS));
  } else {
    GUI.drawList(renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight},
                 static_cast<int>(rows.size()), selectorIndex, [this](int index) { return getRowTitle(index); });
  }

  const bool hasBookmarks = !rows.empty() && !isSeparatorRow(selectorIndex);
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), hasBookmarks ? tr(STR_OPEN) : "",
                                            hasBookmarks ? tr(STR_RENAME) : "", hasBookmarks ? tr(STR_DELETE) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  renderer.displayBuffer();
}
