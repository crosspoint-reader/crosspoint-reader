#include "PdfReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

PdfReaderChapterSelectionActivity::PdfReaderChapterSelectionActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput,
    const PdfFixedVector<PdfOutlineEntry, PDF_MAX_OUTLINE_ENTRIES>& outlineIn, uint32_t currentPageZeroBased)
    : Activity("PdfChapterSelection", renderer, mappedInput), outline(outlineIn), currentPage(currentPageZeroBased) {}

int PdfReaderChapterSelectionActivity::getTotalItems() const { return static_cast<int>(outline.size()); }

int PdfReaderChapterSelectionActivity::getPageItems() const {
  constexpr int lineHeight = 30;
  const int screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = 60 + hintGutterHeight;
  const int availableHeight = screenHeight - startY - lineHeight;
  return std::max(1, availableHeight / lineHeight);
}

void PdfReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  for (size_t i = 0; i < outline.size(); ++i) {
    if (outline[i].pageNum <= currentPage) {
      selectedIndex = static_cast<int>(i);
    }
  }
  requestUpdate();
}

void PdfReaderChapterSelectionActivity::onExit() { Activity::onExit(); }

void PdfReaderChapterSelectionActivity::loop() {
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex >= 0 && selectedIndex < totalItems) {
      setResult(PageResult{outline[static_cast<size_t>(selectedIndex)].pageNum});
    } else {
      ActivityResult r;
      r.isCancelled = true;
      setResult(std::move(r));
    }
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult r;
    r.isCancelled = true;
    setResult(std::move(r));
    finish();
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
  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, totalItems, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, totalItems, pageItems);
    requestUpdate();
  });
}

void PdfReaderChapterSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  const int titleW = renderer.getTextWidth(UI_12_FONT_ID, tr(STR_SELECT_CHAPTER), EpdFontFamily::BOLD);
  const int titleX = contentX + (contentWidth - titleW) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_SELECT_CHAPTER), true, EpdFontFamily::BOLD);

  if (totalItems == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, 120, tr(STR_PDF_OUTLINE_EMPTY));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int pageStartIndex = (selectedIndex / pageItems) * pageItems;
  renderer.fillRect(contentX, 60 + contentY + (selectedIndex - pageStartIndex) * 30 - 2, contentWidth - 1, 30);

  for (int i = 0; i < pageItems; ++i) {
    const int itemIndex = pageStartIndex + i;
    if (itemIndex >= totalItems) break;
    const int displayY = 60 + contentY + i * 30;
    const bool isSelected = (itemIndex == selectedIndex);

    char pageBuf[48];
    snprintf(pageBuf, sizeof(pageBuf), " p.%u",
             static_cast<unsigned>(outline[static_cast<size_t>(itemIndex)].pageNum + 1));
    std::string line = std::string(outline[static_cast<size_t>(itemIndex)].title.c_str()) + pageBuf;
    line = renderer.truncatedText(UI_10_FONT_ID, line.c_str(), contentWidth - 40);

    renderer.drawText(UI_10_FONT_ID, contentX + 20, displayY, line.c_str(), !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
