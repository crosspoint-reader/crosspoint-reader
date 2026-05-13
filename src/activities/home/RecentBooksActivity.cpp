#include "RecentBooksActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "../ActivityManager.h"
#include "../util/ConfirmationActivity.h"
#include "BookInfoActivity.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(books.size());

  for (const auto& book : books) {
    // Skip if file no longer exists
    if (!Storage.exists(book.path.c_str())) {
      continue;
    }
    recentBooks.push_back(book);
  }
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  if (initialFocusIndex >= 0 && initialFocusIndex < static_cast<int>(recentBooks.size())) {
    selectorIndex = initialFocusIndex;
  }
  initialFocusIndex = -1;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
}

void RecentBooksActivity::loop() {
  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Confirm &&
        (ev.type == ButtonEventManager::PressType::Short || ev.type == ButtonEventManager::PressType::Long)) {
      if (recentBooks.empty() || selectorIndex >= static_cast<int>(recentBooks.size())) {
        return;
      }
      // Long-press Confirm signals "open with KOReader sync" only for EPUBs.
      // Short-press is unchanged direct open.
      const bool longPress = (ev.type == ButtonEventManager::PressType::Long) && KOREADER_STORE.hasCredentials();
      const std::string& selectedPath = recentBooks[selectorIndex].path;
      const bool isEpubBook = FsHelpers::hasEpubExtension(selectedPath);
      LOG_DBG("RBA", "Selected recent book: %s (sync=%d epub=%d)", selectedPath.c_str(), longPress ? 1 : 0,
              isEpubBook ? 1 : 0);
      if (longPress && isEpubBook) {
        auto& sync = APP_STATE.koReaderSyncSession;
        sync.autoPullEpubPath = selectedPath;
        sync.exitToHomeAfterSync = false;
        APP_STATE.saveToFile();
      }
      ReturnHint hint;
      hint.target = ReturnTo::RecentBooks;
      hint.selectIndex = selectorIndex;
      activityManager.replaceWithReader(recentBooks[selectorIndex].path, std::move(hint));
      return;
    }

    if (ev.button == MappedInputManager::Button::Back && ev.type == ButtonEventManager::PressType::Short) {
      onGoHome();
      return;
    }

    if (ev.button == MappedInputManager::Button::Left && ev.type == ButtonEventManager::PressType::Short) {
      if (recentBooks.empty() || selectorIndex >= static_cast<int>(recentBooks.size())) return;
      const std::string bookPath = recentBooks[selectorIndex].path;
      const std::string bookTitle = recentBooks[selectorIndex].title;

      auto handler = [this, bookPath](const ActivityResult& res) {
        if (!res.isCancelled) {
          LOG_DBG("RBA", "Removing from recent books: %s", bookPath.c_str());
          RECENT_BOOKS.removeBook(bookPath);
          loadRecentBooks();
          if (recentBooks.empty()) {
            selectorIndex = 0;
          } else if (selectorIndex >= static_cast<int>(recentBooks.size())) {
            selectorIndex = static_cast<int>(recentBooks.size()) - 1;
          }
          requestUpdate(true);
        } else {
          LOG_DBG("RBA", "Remove cancelled by user");
        }
      };

      std::string heading = tr(STR_REMOVE) + std::string("? ");
      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, bookTitle),
                             handler);
      return;
    }

    if (ev.button == MappedInputManager::Button::Right && ev.type == ButtonEventManager::PressType::Short) {
      if (recentBooks.empty() || selectorIndex >= static_cast<int>(recentBooks.size())) return;
      const std::string& path = recentBooks[selectorIndex].path;
      if (FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path)) {
        startActivityForResult(std::make_unique<BookInfoActivity>(renderer, mappedInput, path),
                               [this](const ActivityResult&) { requestUpdate(); });
        return;
      }
    }
  }

  // Navigator is restricted to Up/Down so it cannot race the Left/Right Short
  // handlers above (Left/Right actions are not in the onNextList/onPreviousList button sets).
  const int listSize = static_cast<int>(recentBooks.size());
  buttonNavigator.onNextList({MappedInputManager::Button::Down}, selectorIndex, listSize, [this] { requestUpdate(); });
  buttonNavigator.onPreviousList({MappedInputManager::Button::Up}, selectorIndex, listSize,
                                 [this] { requestUpdate(); });
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_MENU_RECENT_BOOKS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;

  // Recent tab
  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, contentTop + 20,
                      tr(STR_NO_RECENT_BOOKS));
  } else {
    GUI.drawList(
        renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight}, recentBooks.size(), selectorIndex,
        [this](int index) { return recentBooks[index].title; },
        [this](int index) {
          const auto& book = recentBooks[index];
          if (!book.author.empty() && !book.series.empty()) return book.author + "\n" + book.series;
          if (!book.series.empty()) return book.series;
          return book.author;
        },
        [this](int index) { return UITheme::getFileIcon(recentBooks[index].path); });
  }

  // Help text
  const bool hasInfo = !recentBooks.empty() && selectorIndex < recentBooks.size() &&
                       (FsHelpers::hasEpubExtension(recentBooks[selectorIndex].path) ||
                        FsHelpers::hasXtcExtension(recentBooks[selectorIndex].path));
  const bool hasBooks = !recentBooks.empty();
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), hasBooks ? tr(STR_OPEN) : "", hasBooks ? tr(STR_REMOVE) : "",
                                            hasInfo ? tr(STR_INFO) : "");

  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // Side buttons (Up/Down) navigate; show their hints on the side
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  renderer.displayBuffer();
}
