#include "ReadingProgressActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <functional>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "fontIds.h"

// book.bin layout (must stay in sync with BookMetadataCache.cpp):
//   [0]   uint8_t  version
//   [1-4] uint32_t lutOffset
//   [5-6] uint16_t spineCount
//   [7-8] uint16_t tocCount
static constexpr uint8_t kBookCacheVersion = 8;
static constexpr char kCacheDir[] = "/.crosspoint";

// Read the progress percent (0-100) for a book from its two cache files.
// Returns 0 if any file is missing, version-mismatched, or unreadable.
static int loadProgressPercent(const std::string& bookPath) {
  const std::string cacheDir =
      std::string(kCacheDir) + "/epub_" + std::to_string(std::hash<std::string>{}(bookPath));

  // Read spineIndex from progress.bin (bytes 0-1, little-endian uint16_t)
  {
    HalFile f;
    if (!Storage.openFileForRead("RPA", (cacheDir + "/progress.bin").c_str(), f) || !f) return 0;
    uint8_t buf[2];
    if (f.read(buf, 2) < 2) return 0;
    const uint16_t spineIndex = static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);

    // Read spineCount from book.bin header (9 bytes: version + lutOffset + spineCount)
    HalFile bf;
    if (!Storage.openFileForRead("RPA", (cacheDir + "/book.bin").c_str(), bf) || !bf) return 0;
    uint8_t hdr[7];
    if (bf.read(hdr, 7) < 7) return 0;
    if (hdr[0] != kBookCacheVersion) return 0;
    const uint16_t spineCount =
        static_cast<uint16_t>(hdr[5]) | (static_cast<uint16_t>(hdr[6]) << 8);
    if (spineCount == 0) return 0;

    const int pct = static_cast<int>(spineIndex) * 100 / static_cast<int>(spineCount);
    return pct < 100 ? pct : 100;
  }
}

void ReadingProgressActivity::loadEntries() {
  const auto& books = RECENT_BOOKS.getBooks();
  entries.clear();
  entries.reserve(books.size());
  for (const auto& book : books) {
    Entry e;
    e.title = book.title.empty() ? book.path : book.title;
    e.percent = loadProgressPercent(book.path);
    entries.push_back(std::move(e));
  }
}

void ReadingProgressActivity::onEnter() {
  Activity::onEnter();
  loadEntries();
  selectedIndex = 0;
  requestUpdate();
}

void ReadingProgressActivity::onExit() {
  entries.clear();
  entries.shrink_to_fit();
  Activity::onExit();
}

void ReadingProgressActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const int total = static_cast<int>(entries.size());

  buttonNavigator.onNextRelease([this, total] {
    if (selectedIndex < total - 1) {
      selectedIndex++;
      requestUpdate();
    }
  });

  buttonNavigator.onPreviousRelease([this] {
    if (selectedIndex > 0) {
      selectedIndex--;
      requestUpdate();
    }
  });

  // Page-jump on hold
  buttonNavigator.onNextContinuous([this, total] {
    if (selectedIndex < total - 1) {
      selectedIndex++;
      requestUpdate();
    }
  });

  buttonNavigator.onPreviousContinuous([this] {
    if (selectedIndex > 0) {
      selectedIndex--;
      requestUpdate();
    }
  });
}

void ReadingProgressActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  const int sidePad = metrics.contentSidePadding;

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight},
                 tr(STR_READING_PROGRESS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = h - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentH = contentBottom - contentTop;

  // Use the reader font for titles: it's whatever the user has selected (e.g. Amiri),
  // so Arabic and other non-Latin titles render correctly. Fall back to UI font for
  // the percent label which is always ASCII.
  const int titleFontId = SETTINGS.getReaderFontId();

  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentH / 2,
                              tr(STR_NO_RECENT_BOOKS));
  } else {
    const int lineH = renderer.getLineHeight(titleFontId);
    // Entry: title line + 4px gap + progress bar + bottom spacing
    const int entryH = lineH + 4 + metrics.progressBarHeight + metrics.verticalSpacing;
    const int pageItems = contentH / entryH;

    const int scrollTop = (pageItems > 0) ? (selectedIndex / pageItems) * pageItems : 0;
    int y = contentTop;

    for (int i = scrollTop; i < static_cast<int>(entries.size()) && i < scrollTop + pageItems;
         ++i) {
      const auto& e = entries[i];
      const bool selected = (i == selectedIndex);
      const auto style = selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

      // Percent label (ASCII) — right-aligned, using UI font for consistent sizing
      char pctBuf[6];
      snprintf(pctBuf, sizeof(pctBuf), "%d%%", e.percent);
      const int pctW = renderer.getTextWidth(UI_10_FONT_ID, pctBuf);
      renderer.drawText(UI_10_FONT_ID, w - sidePad - pctW, y, pctBuf, true);

      // Title — uses reader font so Arabic/non-Latin scripts display correctly
      const int maxTitleW = w - sidePad * 2 - pctW - metrics.verticalSpacing;
      const std::string title = renderer.truncatedText(titleFontId, e.title.c_str(), maxTitleW);
      renderer.drawText(titleFontId, sidePad, y, title.c_str(), true, style);

      y += lineH + 4;

      // Progress bar
      GUI.drawProgressBar(renderer,
                          Rect{sidePad, y, w - sidePad * 2, metrics.progressBarHeight},
                          e.percent, 100);

      y += metrics.progressBarHeight + metrics.verticalSpacing;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
