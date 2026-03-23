#include "RecentBooksActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "../util/ConfirmationActivity.h"
#include "BookInfoActivity.h"
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
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
}

void RecentBooksActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !recentBooks.empty() &&
      selectorIndex < static_cast<int>(recentBooks.size())) {
    LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
    onSelectBook(recentBooks[selectorIndex].path);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  }

  // Left button: remove selected book from recent list
  if (!recentBooks.empty() && selectorIndex < recentBooks.size() &&
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    const std::string bookPath = recentBooks[selectorIndex].path;
    const std::string bookTitle = recentBooks[selectorIndex].title;

    auto handler = [this, bookPath](const ActivityResult& res) {
      if (!res.isCancelled) {
        LOG_DBG("RBA", "Removing from recent books: %s", bookPath.c_str());
        RECENT_BOOKS.removeBook(bookPath);
        loadRecentBooks();
        if (recentBooks.empty()) {
          selectorIndex = 0;
        } else if (selectorIndex >= recentBooks.size()) {
          selectorIndex = recentBooks.size() - 1;
        }
        requestUpdate(true);
      } else {
        LOG_DBG("RBA", "Remove cancelled by user");
      }
    };

    std::string heading = tr(STR_REMOVE) + std::string("? ");
    startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, bookTitle), handler);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) && !recentBooks.empty() &&
      selectorIndex < static_cast<int>(recentBooks.size())) {
    const std::string& path = recentBooks[selectorIndex].path;
    if (FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path)) {
      startActivityForResult(std::make_unique<BookInfoActivity>(renderer, mappedInput, path),
                             [this](const ActivityResult&) { requestUpdate(); });
      return;
    }
  }

  int listSize = static_cast<int>(recentBooks.size());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
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
