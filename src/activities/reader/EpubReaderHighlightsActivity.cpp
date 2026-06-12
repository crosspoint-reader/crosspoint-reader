#include "EpubReaderHighlightsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <util/HighlightUtil.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int ENTER_DELETE_MODE_MS = 700;
constexpr int DELETE_MODE_OFF = 0;
constexpr int DELETE_MODE_DISPLAY = 1;
constexpr int DELETE_MODE_CONFIRM = 2;

// Layout constants used in renderScreen
constexpr int LINE_HEIGHT = 60;
}  // namespace

void EpubReaderHighlightsActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  const std::string path = HighlightUtil::getHighlightPath(epubPath);
  if (Storage.exists(path.c_str())) {
    String json = Storage.readFile(path.c_str());
    if (json.isEmpty()) {
      LOG_ERR("EPH", "Failed to load highlights from %s. Empty highlight file", path.c_str());
      highlights.clear();
      highlights.shrink_to_fit();
    } else {
      JsonSettingsIO::loadHighlights(highlights, json.c_str());

      // Present highlights in reading order (chapter, then position), matching the web export.
      std::sort(highlights.begin(), highlights.end(), [](const HighlightEntry& a, const HighlightEntry& b) {
        if (a.spineIndex != b.spineIndex) return a.spineIndex < b.spineIndex;
        return a.percentage < b.percentage;
      });
    }
  } else {
    LOG_DBG("EPH", "No highlight file found at %s, starting with empty highlights", path.c_str());
    highlights.clear();
    highlights.shrink_to_fit();
  }
  LOG_DBG("EPH", "Loaded %d highlights for book: %s", static_cast<int>(highlights.size()), epubPath.c_str());

  // Trigger first update
  requestUpdate();
}

void EpubReaderHighlightsActivity::onExit() { Activity::onExit(); }

int EpubReaderHighlightsActivity::getGutterBottom(const GfxRenderer& renderer) {
  const auto orientation = renderer.getOrientation();
  const bool isPortrait = orientation == GfxRenderer::Orientation::Portrait;
  return isPortrait ? 75 : 40;  // Reserve vertical space for button hints at the bottom
}

int EpubReaderHighlightsActivity::getListHeight(const GfxRenderer& renderer) {
  const auto pageHeight = renderer.getScreenHeight();
  return pageHeight - getGutterBottom(renderer) - LINE_HEIGHT;  // Reserve vertical space for title and button hints
}

void EpubReaderHighlightsActivity::loop() {
  // Delete confirmation mode
  if (confirmingDelete >= DELETE_MODE_DISPLAY) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (confirmingDelete == DELETE_MODE_DISPLAY) {
        confirmingDelete = DELETE_MODE_CONFIRM;  // first confirmation, update text
        requestUpdate();
        return;
      }
      highlights.erase(highlights.begin() + selectorIndex);
      const std::string path = HighlightUtil::getHighlightPath(epubPath);
      Storage.mkdir(HighlightUtil::getHighlightsDir().c_str());
      if (!JsonSettingsIO::saveHighlights(highlights, path.c_str())) {
        LOG_ERR("EPH", "Failed to save highlights after delete");
      }

      // Move selector up if we deleted the last item
      if (selectorIndex >= highlights.size() && selectorIndex > 0) {
        selectorIndex--;
      }

      requestUpdate();
      confirmingDelete = DELETE_MODE_OFF;
      return;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      requestUpdate();
      confirmingDelete = DELETE_MODE_OFF;
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {  // Open
    if (highlights.empty()) {
      return;
    }
    const auto& highlight = highlights.at(selectorIndex);
    CrossPointPosition pos = ProgressMapper::toCrossPoint(epub, {highlight.xpath, highlight.percentage}, renderer);
    setResult(ProgressChangeResult{pos.spineIndex, pos.pageNumber});
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() > ENTER_DELETE_MODE_MS) {
    if (highlights.empty()) {
      return;
    }
    confirmingDelete = DELETE_MODE_DISPLAY;
    requestUpdate();
  }

  buttonNavigator.onNextRelease([this] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, highlights.size());
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, highlights.size());
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, highlights.size(),
                                                   GUI.getListPageItems(getListHeight(renderer), true));
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, highlights.size(),
                                                       GUI.getListPageItems(getListHeight(renderer), true));
    requestUpdate();
  });
}

void EpubReaderHighlightsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  // Landscape orientation: reserve a horizontal gutter for button hints.
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Inverted portrait: reserve vertical space for hints at the top.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 40 : 0;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int hintGutterBottom = getGutterBottom(renderer);
  const int contentY = hintGutterHeight;
  const int listY = contentY + LINE_HEIGHT;  // Reserve vertical space for title
  const int listHeight = getListHeight(renderer);
  const int numHighlights = highlights.size();

  // Manual centering to honor content gutters.
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_HIGHLIGHTS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_HIGHLIGHTS), true, EpdFontFamily::BOLD);

  const auto getHighlightTitle = [this](int index) {
    return highlights.at(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index).text;
  };
  const auto getHighlightSubtitle = [this](int index) {
    const auto& highlight = highlights.at(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index);
    auto tocIndex = epub->getTocIndexForSpineIndex(highlight.spineIndex);
    auto tocTitle = (tocIndex >= 0) ? (epub->getTocItem(tocIndex)).title : tr(STR_UNNAMED);
    const int percent = static_cast<int>(highlight.percentage * 100.0f + 0.5f);
    return std::to_string(percent) + "% - " + tocTitle;
  };

  if (numHighlights > 0) {
    if (confirmingDelete >= DELETE_MODE_DISPLAY) {
      GUI.drawHelpText(renderer, Rect{0, pageHeight / 2 - LINE_HEIGHT * 2, contentWidth, LINE_HEIGHT},
                       tr(STR_CONFIRM_DELETE_HIGHLIGHT));

      // render list with just the selected item for the user to confirm to delete
      GUI.drawList(renderer, Rect{contentX, pageHeight / 2, contentWidth, LINE_HEIGHT}, 1, 0, getHighlightTitle,
                   getHighlightSubtitle);
    } else {
      GUI.drawList(renderer, Rect{contentX, listY, contentWidth, listHeight}, numHighlights, selectorIndex,
                   getHighlightTitle, getHighlightSubtitle);

      GUI.drawHelpText(renderer, Rect{contentX, pageHeight - hintGutterBottom, contentWidth, LINE_HEIGHT},
                       tr(STR_HOLD_CONFIRM_TO_DELETE));
    }
  } else {
    GUI.drawHelpText(renderer, Rect{contentX, LINE_HEIGHT * 2, contentWidth, LINE_HEIGHT},
                     tr(STR_HIGHLIGHT_INSTRUCTIONS));
  }

  const auto backLabel = confirmingDelete >= DELETE_MODE_DISPLAY ? tr(STR_CANCEL) : tr(STR_BACK);
  const auto confirmLabel =
      highlights.size() > 0 ? (confirmingDelete >= DELETE_MODE_DISPLAY ? tr(STR_DELETE) : tr(STR_OPEN)) : "";
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
