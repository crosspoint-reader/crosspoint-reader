#include "EpubReaderSearchActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Repaint the progress screen only once the percentage has advanced this much,
// keeping e-ink refreshes bounded now that progress moves per page.
constexpr int PROGRESS_REPAINT_STEP_PERCENT = 10;
}  // namespace

EpubReaderSearchActivity::EpubReaderSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   const std::shared_ptr<Epub>& epub, const char* query,
                                                   const int startSpineIndex, const int startPage, const int stopPage,
                                                   const uint16_t viewportWidth, const uint16_t viewportHeight)
    : Activity("EpubReaderSearch", renderer, mappedInput),
      epub(epub),
      section(this->epub, startSpineIndex, renderer),
      startSpineIndex(startSpineIndex),
      startPage(startPage),
      stopPage(stopPage),
      currentSpineIndex(startSpineIndex),
      currentPage(startPage),
      viewportWidth(viewportWidth),
      viewportHeight(viewportHeight) {
  if (query) {
    const size_t length = std::min(strlen(query), this->query.size() - 1);
    memcpy(this->query.data(), query, length);
    this->query[length] = '\0';
  }
  // Build the KMP failure function once here; every page scan reuses it. A
  // rejected query (empty or oversized) means there is nothing to scan, so fail
  // closed rather than relying solely on the caller having pre-validated it.
  if (!Section::buildSearchPrefix(this->query.data(), queryPrefix)) {
    state = SearchState::NotFound;
  }
}

void EpubReaderSearchActivity::onEnter() {
  Activity::onEnter();
  // Paint the status screen before an uncached chapter starts its potentially
  // long layout pass.
  requestUpdateAndWait();
}

void EpubReaderSearchActivity::onExit() { Activity::onExit(); }

bool EpubReaderSearchActivity::skipLoopDelay() { return state == SearchState::Searching; }

bool EpubReaderSearchActivity::preventAutoSleep() { return state == SearchState::Searching; }

void EpubReaderSearchActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void EpubReaderSearchActivity::setFailure(const SearchState failureState) {
  state = failureState;
  requestUpdate();
}

bool EpubReaderSearchActivity::reachedWrappedStop() const {
  if (!wrapped) {
    return false;
  }
  return currentSpineIndex > startSpineIndex || (currentSpineIndex == startSpineIndex && currentPage >= stopPage);
}

void EpubReaderSearchActivity::advanceSpine() {
  ++currentSpineIndex;
  currentPage = 0;
  sectionLoaded = false;
}

bool EpubReaderSearchActivity::loadCurrentSection() {
  section.resetForSpine(currentSpineIndex);
  if (section.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                              SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                              viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                              SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    sectionLoaded = true;
    return true;
  }

  LOG_DBG("EPS", "Building section %d for search", currentSpineIndex);
  if (!section.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                 SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                 viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                 SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    LOG_ERR("EPS", "Failed to build section %d for search", currentSpineIndex);
    return false;
  }

  sectionLoaded = true;
  return true;
}

bool EpubReaderSearchActivity::preparePage() {
  const int spineCount = epub ? epub->getSpineItemsCount() : 0;
  if (spineCount <= 0) {
    setFailure(SearchState::Error);
    return false;
  }

  while (true) {
    if (currentSpineIndex >= spineCount) {
      if (wrapped) {
        setFailure(SearchState::NotFound);
        return false;
      }
      wrapped = true;
      currentSpineIndex = 0;
      currentPage = 0;
      sectionLoaded = false;
    }

    if (reachedWrappedStop()) {
      setFailure(SearchState::NotFound);
      return false;
    }

    if (!sectionLoaded && !loadCurrentSection()) {
      setFailure(SearchState::Error);
      return false;
    }

    if (currentPage >= 0 && currentPage < section.pageCount) {
      return true;
    }

    advanceSpine();
  }
}

void EpubReaderSearchActivity::scanNextPage() {
  if (!preparePage()) {
    return;
  }

  const auto match = section.pageContainsText(static_cast<uint16_t>(currentPage), query.data(), queryPrefix);
  if (!match.has_value()) {
    setFailure(SearchState::Error);
    return;
  }
  if (*match) {
    setResult(ProgressChangeResult{currentSpineIndex, currentPage});
    finish();
    return;
  }

  ++currentPage;
}

int EpubReaderSearchActivity::searchProgressPercent() const {
  const int spineCount = epub ? epub->getSpineItemsCount() : 0;
  if (spineCount <= 0) {
    return 0;
  }
  // Whole-spine position across the scan: forward to the last spine, then
  // through the wrapped spines back toward the start position.
  const int spinePosition =
      wrapped ? (spineCount - startSpineIndex) + currentSpineIndex : currentSpineIndex - startSpineIndex;
  // Interpolate within the current spine so progress advances per page rather
  // than only at chapter boundaries — for a single/few-spine book the
  // percentage would otherwise sit frozen for the whole scan of a long spine.
  float pageFraction = 0.0f;
  if (section.pageCount > 0) {
    pageFraction = static_cast<float>(std::min<int>(currentPage, section.pageCount)) / section.pageCount;
  }
  const float linearPos = static_cast<float>(std::max(0, spinePosition)) + pageFraction;
  return ReaderUtils::clampPercent(static_cast<int>((linearPos * 100.0f) / spineCount));
}

void EpubReaderSearchActivity::loop() {
  switch (state) {
    case SearchState::Searching:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        cancel();
        return;
      }
      scanNextPage();
      // Progress is now page-granular, so only repaint once it has advanced a
      // whole step. This bounds e-ink refreshes to ~100/step over an entire
      // scan regardless of book structure, instead of one per page.
      if (state == SearchState::Searching) {
        const int percent = searchProgressPercent();
        if (percent - lastProgressPercent >= PROGRESS_REPAINT_STEP_PERCENT) {
          lastProgressPercent = percent;
          requestUpdate();
        }
      }
      return;

    case SearchState::NotFound:
    case SearchState::Error:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        cancel();
      }
      return;
  }
}

void EpubReaderSearchActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_SEARCH));
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      query.data());

  const char* message = nullptr;
  switch (state) {
    case SearchState::Searching:
      message = tr(STR_SEARCHING_BOOK);
      break;
    case SearchState::NotFound:
      message = tr(STR_NO_SEARCH_RESULTS);
      break;
    case SearchState::Error:
      message = tr(STR_ERROR_GENERAL_FAILURE);
      break;
  }

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight;
  const int messageY = contentTop + (screen.height - contentTop) / 2;
  UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID, messageY, message, true, EpdFontFamily::BOLD);

  if (state == SearchState::Searching) {
    char percentText[8];
    snprintf(percentText, sizeof(percentText), "%d%%", searchProgressPercent());
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID, messageY + renderer.getLineHeight(UI_12_FONT_ID),
                              percentText, true);
  }

  const char* backLabel = state == SearchState::Searching ? tr(STR_CANCEL) : tr(STR_BACK);
  const auto labels = mappedInput.mapLabels(backLabel, "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
