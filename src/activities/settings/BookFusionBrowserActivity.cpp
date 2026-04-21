#include "BookFusionBrowserActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "BookFusionBookIdStore.h"
#include "BookFusionTokenStore.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/StringUtils.h"

namespace {
constexpr int PAGE_ITEMS = 21;  // 20 books + optional "Next page" sentinel

struct Category {
  StrId nameId;
  const char* list;
  const char* sort;
};

constexpr Category CATEGORIES[] = {
    {StrId::STR_BF_CURRENTLY_READING, "currently_reading", "last_read_at-desc"},
    {StrId::STR_BF_FAVORITES, "favorites", nullptr},
    {StrId::STR_BF_PLAN_TO_READ, "planned_to_read", nullptr},
    {StrId::STR_BF_COMPLETED, "completed", nullptr},
    {StrId::STR_BF_ALL_BOOKS, nullptr, nullptr},
};
constexpr int NUM_CATEGORIES = sizeof(CATEGORIES) / sizeof(CATEGORIES[0]);
}  // namespace

void BookFusionBrowserActivity::onEnter() {
  Activity::onEnter();

  if (!BF_TOKEN_STORE.hasToken()) {
    state = ERROR;
    strlcpy(errorMsg, tr(STR_BF_NO_TOKEN_MSG), sizeof(errorMsg));
    requestUpdate();
    return;
  }

  state = CATEGORY_SELECTION;
  requestUpdate();
}

void BookFusionBrowserActivity::handleCategorySelection() {
  currentCategory = selectedCategory;
  currentPage = 1;

  if (WiFi.status() == WL_CONNECTED) {
    loadPage(1);
    return;
  }

  state = WIFI_SELECTION;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookFusionBrowserActivity::onExit() {
  Activity::onExit();
  WiFi.mode(WIFI_OFF);
}

void BookFusionBrowserActivity::onWifiSelectionComplete(bool success) {
  if (!success) {
    state = ERROR;
    strlcpy(errorMsg, tr(STR_WIFI_CONN_FAILED), sizeof(errorMsg));
    requestUpdate();
    return;
  }
  currentPage = 1;
  loadPage(1);
}

void BookFusionBrowserActivity::loadPage(int page) {
  {
    RenderLock lock(*this);
    state = LOADING;
    selectedIndex = 0;
  }
  requestUpdate(true);

  const auto& cat = CATEGORIES[currentCategory];
  const auto err = BookFusionSyncClient::searchBooks(page, searchResult, cat.list, cat.sort);

  if (err != BookFusionSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = ERROR;
      strlcpy(errorMsg, BookFusionSyncClient::errorString(err), sizeof(errorMsg));
    }
    requestUpdate();
    return;
  }

  if (searchResult.count == 0) {
    {
      RenderLock lock(*this);
      state = ERROR;
      strlcpy(errorMsg, tr(STR_BF_NO_BOOKS), sizeof(errorMsg));
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = BROWSING;
    currentPage = page;
  }
  requestUpdate();
}

void BookFusionBrowserActivity::startDownload(int bookIndex) {
  const auto& book = searchResult.books[bookIndex];

  {
    RenderLock lock(*this);
    state = DOWNLOADING;
    downloadProgress = 0;
    downloadTotal = 0;
    strlcpy(downloadTitle, book.title, sizeof(downloadTitle));
  }
  requestUpdateAndWait();

  // Fetch the pre-signed download URL from BookFusion.
  const auto urlErr = BookFusionSyncClient::getDownloadUrl(book.id, downloadUrl, sizeof(downloadUrl));
  if (urlErr != BookFusionSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = ERROR;
      if (urlErr == BookFusionSyncClient::NOT_FOUND) {
        strlcpy(errorMsg, tr(STR_BF_BOOK_UNAVAILABLE), sizeof(errorMsg));
      } else {
        strlcpy(errorMsg, BookFusionSyncClient::errorString(urlErr), sizeof(errorMsg));
      }
    }
    requestUpdate();
    return;
  }

  // Build destination path: "/Title - Author.ext" (sanitized).
  std::string baseName = book.title;
  if (book.authors[0] != '\0') {
    baseName += " - ";
    baseName += book.authors;
  }

  char ext[8] = "epub";
  if (book.format[0] != '\0') {
    size_t i = 0;
    for (; i < sizeof(ext) - 1 && book.format[i] != '\0'; i++) {
      ext[i] = static_cast<char>(tolower(static_cast<unsigned char>(book.format[i])));
    }
    ext[i] = '\0';
  }

  const std::string filename = "/" + StringUtils::sanitizeFilename(baseName) + "." + ext;
  LOG_DBG("BFB", "Downloading book_id=%lu -> %s", static_cast<unsigned long>(book.id), filename.c_str());

  const auto dlResult =
      HttpDownloader::downloadToFile(downloadUrl, filename, [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        requestUpdate(true);
      });

  if (dlResult != HttpDownloader::OK) {
    {
      RenderLock lock(*this);
      state = ERROR;
      strlcpy(errorMsg, tr(STR_DOWNLOAD_FAILED), sizeof(errorMsg));
    }
    requestUpdate();
    return;
  }

  // Save sidecar so BookFusionSyncActivity can find the book_id for this file.
  BookFusionBookIdStore::saveBookId(filename.c_str(), book.id);

  // Invalidate any stale EPUB cache for this path.
  Epub epub(filename, "/.crosspoint");
  epub.clearCache();

  LOG_DBG("BFB", "Download complete, sidecar saved for book_id=%lu", static_cast<unsigned long>(book.id));

  {
    RenderLock lock(*this);
    state = DOWNLOAD_COMPLETE;
  }
  requestUpdate(true);
}

void BookFusionBrowserActivity::loop() {
  if (state == WIFI_SELECTION || state == LOADING || state == DOWNLOADING) {
    return;
  }

  if (state == CATEGORY_SELECTION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      handleCategorySelection();
      return;
    }
    buttonNavigator.onNextRelease([this] {
      selectedCategory = ButtonNavigator::nextIndex(selectedCategory, NUM_CATEGORIES);
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this] {
      selectedCategory = ButtonNavigator::previousIndex(selectedCategory, NUM_CATEGORIES);
      requestUpdate();
    });
    return;
  }

  if (state == ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (BF_TOKEN_STORE.hasToken()) {
        {
          RenderLock lock(*this);
          state = CATEGORY_SELECTION;
        }
        requestUpdate();
      } else {
        finish();
      }
    }
    return;
  }

  if (state == DOWNLOAD_COMPLETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state = BROWSING;
      }
      requestUpdate();
    }
    return;
  }

  if (state == BROWSING) {
    const int totalItems = searchResult.count + (searchResult.hasMore ? 1 : 0);

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (currentPage > 1) {
        loadPage(currentPage - 1);
      } else {
        {
          RenderLock lock(*this);
          state = CATEGORY_SELECTION;
        }
        requestUpdate();
      }
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (selectedIndex < searchResult.count) {
        startDownload(selectedIndex);
      } else if (searchResult.hasMore) {
        loadPage(currentPage + 1);
      }
      return;
    }

    buttonNavigator.onNextRelease([this, totalItems] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
      requestUpdate();
    });

    buttonNavigator.onPreviousRelease([this, totalItems] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
      requestUpdate();
    });

    buttonNavigator.onNextContinuous([this, totalItems] {
      selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, totalItems, PAGE_ITEMS);
      requestUpdate();
    });

    buttonNavigator.onPreviousContinuous([this, totalItems] {
      selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, totalItems, PAGE_ITEMS);
      requestUpdate();
    });
  }
}

void BookFusionBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const char* headerTitle =
      (state == CATEGORY_SELECTION) ? tr(STR_BF_BROWSE_LIBRARY) : I18N.get(CATEGORIES[currentCategory].nameId);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerTitle);

  if (state == CATEGORY_SELECTION) {
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, NUM_CATEGORIES, selectedCategory,
        [](int index) -> std::string { return std::string(I18N.get(CATEGORIES[index].nameId)); }, nullptr, nullptr,
        nullptr, true);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == WIFI_SELECTION || state == LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_LOADING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, errorMsg, true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
    const int maxWidth = pageWidth - 40;
    auto title = renderer.truncatedText(UI_10_FONT_ID, downloadTitle, maxWidth);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    if (downloadTotal > 0) {
      constexpr int barX = 50;
      constexpr int barHeight = 20;
      const int barWidth = pageWidth - 100;
      const int barY = pageHeight / 2 + 20;
      GUI.drawProgressBar(renderer, Rect{barX, barY, barWidth, barHeight}, downloadProgress, downloadTotal);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == DOWNLOAD_COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 15, tr(STR_BF_DOWNLOAD_COMPLETE), true,
                              EpdFontFamily::BOLD);
    const int maxWidth = pageWidth - 40;
    auto title = renderer.truncatedText(UI_10_FONT_ID, downloadTitle, maxWidth);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 15, title.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // BROWSING state — draw paginated book list via UITheme.
  const int totalItems = searchResult.count + (searchResult.hasMore ? 1 : 0);
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalItems, selectedIndex,
      [this](int index) -> std::string {
        if (index >= searchResult.count) return std::string(tr(STR_BF_NEXT_PAGE));
        const auto& book = searchResult.books[index];
        std::string text = book.title;
        if (book.authors[0] != '\0') {
          text += " \xe2\x80\x94 ";  // UTF-8 em-dash
          text += book.authors;
        }
        return text;
      },
      nullptr, nullptr, nullptr, true);

  const char* confirmLabel = (selectedIndex >= searchResult.count) ? tr(STR_OPEN) : tr(STR_DOWNLOAD);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
