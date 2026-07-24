#include "RecentBooksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Hold threshold for book actions; the popup appears before the button is released.
constexpr unsigned long LONG_PRESS_MS = 500;
constexpr unsigned long POPUP_DURATION_MS = 1500;

std::string fallbackBookTitle(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  std::string title = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t extension = title.find_last_of('.');
  if (extension != std::string::npos) title.resize(extension);
  std::replace(title.begin(), title.end(), '_', ' ');
  return title;
}
}  // namespace

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  pinnedFlags.clear();
  const auto& storedRecent = RECENT_BOOKS.getBooks();

  recentBooks.reserve(RECENT_BOOKS.getPinnedPaths().size() + storedRecent.size());
  pinnedFlags.reserve(RECENT_BOOKS.getPinnedPaths().size() + storedRecent.size());
  for (const auto& path : RECENT_BOOKS.getPinnedPaths()) {
    const auto metadata = std::find_if(storedRecent.begin(), storedRecent.end(),
                                       [&path](const RecentBook& book) { return book.path == path; });
    if (metadata != storedRecent.end()) {
      recentBooks.push_back(*metadata);
    } else {
      recentBooks.push_back({path, fallbackBookTitle(path), "", ""});
    }
    pinnedFlags.push_back(true);
  }
  for (const auto& book : storedRecent) {
    if (RECENT_BOOKS.isPinned(book.path)) continue;
    recentBooks.push_back(book);
    pinnedFlags.push_back(false);
  }
}

void RecentBooksActivity::clearSearch(const bool preserveQuery) {
  searchActive = false;
  searchResultsTruncated = false;
  if (!preserveQuery) searchQuery.clear();
  searchResults.clear();
}

size_t RecentBooksActivity::visibleItemCount() const {
  if (searchActive) return searchResults.size();
  return recentBooks.empty() ? 0 : recentBooks.size() + 1;
}

bool RecentBooksActivity::isSearchRow(const size_t index) const {
  return !searchActive && !recentBooks.empty() && index == 0;
}

size_t RecentBooksActivity::sourceIndex(const size_t visibleIndex) const {
  if (searchActive) return visibleIndex < searchResults.size() ? searchResults[visibleIndex] : recentBooks.size();
  if (visibleIndex == 0) return recentBooks.size();
  return visibleIndex - 1;
}

void RecentBooksActivity::launchSearch() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH_BOOKS), searchQuery,
                                              BOOK_SEARCH_QUERY_BYTES),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) applySearch(std::get<KeyboardResult>(result.data).text);
        requestUpdate();
      });
}

void RecentBooksActivity::applySearch(const std::string& query) {
  const BookSearchQuery normalized = makeBookSearchQuery(query);
  if (normalized.empty()) {
    clearSearch();
    selectorIndex = recentBooks.empty() ? 0 : 1;
    return;
  }

  searchActive = true;
  searchQuery = query;
  searchResultsTruncated = false;
  searchResults.clear();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int fullPageCapacity = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, true, 0);
  const int noticeReserved = renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  const int truncatedPageCapacity =
      UITheme::getNumberOfItemsPerPage(renderer, true, false, true, true, noticeReserved);
  const size_t maxResults = static_cast<size_t>(
      std::clamp(fullPageCapacity, 1, static_cast<int>(BOOK_SEARCH_RESULT_HARD_LIMIT)));
  const size_t maxResultsWithNotice = static_cast<size_t>(
      std::clamp(truncatedPageCapacity, 1, static_cast<int>(maxResults)));
  searchResults.reserve(maxResults);
  size_t exactCount = 0;
  for (size_t i = 0; i < recentBooks.size(); i++) {
    const RecentBook& book = recentBooks[i];
    BookSearchMatch rank = matchBookSearch(normalized, book.path);
    rank = std::max(rank, matchBookSearch(normalized, book.title));
    rank = std::max(rank, matchBookSearch(normalized, book.author));
    addRankedBookSearchResult(searchResults, exactCount, searchResultsTruncated, i, rank, maxResults);
  }
  if (searchResultsTruncated && searchResults.size() > maxResultsWithNotice) {
    searchResults.resize(maxResultsWithNotice);
  }
  selectorIndex = 0;
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Prune entries whose backing files are gone; this is one of two interaction
  // points where the persistent store gets cleaned (the other is addBook).
  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  // Load data
  loadRecentBooks();

  selectorIndex = recentBooks.empty() ? 0 : 1;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  pinnedFlags.clear();
  searchResults.clear();
}

void RecentBooksActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);

  if (optionPopup.isActive() && suppressPopupConfirmRelease &&
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    suppressPopupConfirmRelease = false;
    confirmPressSeen = false;
    confirmLongHandled = false;
    return;
  }
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (popupMessage != StrId::STR_NONE_OPT) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
      popupMessage = StrId::STR_NONE_OPT;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmPressSeen = true;
    confirmLongHandled = false;
  }

  if (confirmPressSeen && !confirmLongHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MS && !isSearchRow(selectorIndex)) {
    const size_t source = sourceIndex(selectorIndex);
    if (source < recentBooks.size()) {
      confirmLongHandled = true;
      suppressPopupConfirmRelease = true;
      showBookActions(source);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (confirmLongHandled) {
      confirmPressSeen = false;
      confirmLongHandled = false;
      suppressPopupConfirmRelease = false;
      return;
    }
    const bool accept = confirmPressSeen;
    confirmPressSeen = false;
    if (!accept) return;
    if (isSearchRow(selectorIndex)) {
      launchSearch();
      return;
    }
    const size_t source = sourceIndex(selectorIndex);
    if (source < recentBooks.size()) {
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[source].path.c_str());
      onSelectBook(recentBooks[source].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (searchActive) {
      clearSearch(true);
      selectorIndex = recentBooks.empty() ? 0 : 1;
      requestUpdate();
      return;
    }
    onGoHome();
  }

  int listSize = static_cast<int>(visibleItemCount());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void RecentBooksActivity::showBookActions(const size_t source) {
  if (source >= recentBooks.size()) return;
  const RecentBook book = recentBooks[source];
  const bool inRecent = std::find(RECENT_BOOKS.getBooks().begin(), RECENT_BOOKS.getBooks().end(), book) !=
                        RECENT_BOOKS.getBooks().end();

  std::vector<std::string> options = {RECENT_BOOKS.isPinned(book.path) ? tr(STR_UNPIN_BOOK) : tr(STR_PIN_BOOK)};
  if (inRecent) options.push_back(tr(STR_REMOVE_FROM_RECENTS));
  optionPopup.show(StrId::STR_BOOK_ACTIONS, options, 0, [this, book, inRecent](const int selected) {
    if (selected == 0) {
      const auto result = RECENT_BOOKS.togglePin(book.path);
      if (result == RecentBooksStore::PinResult::Pinned) {
        popupMessage = StrId::STR_BOOK_PINNED;
      } else if (result == RecentBooksStore::PinResult::Unpinned) {
        popupMessage = StrId::STR_BOOK_UNPINNED;
      } else if (result == RecentBooksStore::PinResult::LimitReached) {
        popupMessage = StrId::STR_PIN_LIMIT_REACHED;
      } else {
        popupMessage = StrId::STR_ERROR_GENERAL_FAILURE;
      }
      popupTime = millis();
      loadRecentBooks();
      clearSearch();
      selectorIndex = recentBooks.empty() ? 0 : 1;
      requestUpdate();
      return;
    }
    if (inRecent) promptRemoveBook(book.path, book.title);
  });
  requestUpdate();
}

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBA", "Remove from recents cancelled");
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      LOG_DBG("RBA", "Removed from recents: %s", path.c_str());
      loadRecentBooks();
      clearSearch();
      if (recentBooks.empty()) {
        selectorIndex = 0;
      } else if (selectorIndex >= visibleItemCount()) {
        selectorIndex = visibleItemCount() - 1;
      }
      requestUpdate(true);
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
      std::move(handler));
}

void RecentBooksActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  char searchTitle[BOOK_SEARCH_QUERY_BYTES + 96]{};
  const char* headerTitle = tr(STR_MENU_RECENT_BOOKS);
  if (searchActive) {
    snprintf(searchTitle, sizeof(searchTitle), tr(STR_SEARCH_RESULTS_FORMAT), searchQuery.c_str());
    headerTitle = searchTitle;
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerTitle);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int noticeLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int noticeReserved = searchActive && searchResultsTruncated ? noticeLineHeight + metrics.verticalSpacing : 0;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing - noticeReserved;

  const size_t itemCount = visibleItemCount();
  if (itemCount == 0) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20,
                      searchActive ? tr(STR_NO_SEARCH_RESULTS) : tr(STR_NO_RECENT_BOOKS));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectorIndex,
        [this](int index) {
          if (isSearchRow(index)) return std::string(tr(STR_SEARCH_BOOKS));
          const size_t source = sourceIndex(index);
          return source < recentBooks.size() ? recentBooks[source].title : std::string{};
        },
        [this](int index) {
          if (isSearchRow(index)) return std::string{};
          const size_t source = sourceIndex(index);
          if (source >= recentBooks.size()) return std::string{};
          return pinnedFlags[source] ? std::string(tr(STR_PINNED)) : recentBooks[source].author;
        },
        [this](int index) {
          if (isSearchRow(index)) return UIIcon::Library;
          const size_t source = sourceIndex(index);
          if (source >= recentBooks.size()) return UIIcon::None;
          return pinnedFlags[source] ? UIIcon::Bookmark : UITheme::getFileIcon(recentBooks[source].path);
        });
  }

  if (noticeReserved > 0) {
    const int noticeY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - noticeLineHeight;
    const int noticeWidth = pageWidth - metrics.contentSidePadding * 2;
    const std::string notice = renderer.truncatedText(SMALL_FONT_ID, tr(STR_SEARCH_MORE_RESULTS), noticeWidth);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, noticeY, notice.c_str());
  }

  // Help text
  const char* confirmLabel = itemCount == 0 ? "" : (isSearchRow(selectorIndex) ? tr(STR_SEARCH) : tr(STR_OPEN));
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), confirmLabel, itemCount == 0 ? "" : tr(STR_DIR_UP),
                                            itemCount == 0 ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (popupMessage != StrId::STR_NONE_OPT) {
    GUI.drawPopup(renderer, I18N.get(popupMessage));
  } else {
    renderer.displayBuffer();
  }
}
