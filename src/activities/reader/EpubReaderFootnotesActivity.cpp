#include "EpubReaderFootnotesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void EpubReaderFootnotesActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void EpubReaderFootnotesActivity::onExit() { Activity::onExit(); }

void EpubReaderFootnotesActivity::loop() {
  auto selectFootnote = [this] {
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(footnotes.size())) {
      setResult(FootnoteResult{footnotes[selectedIndex].href});
      finish();
    }
  };

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    selectFootnote();
    return;
  }

  if (!footnotes.empty()) {
    const auto orientation = renderer.getOrientation();
    const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
    const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
    const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
    const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
    const int contentX = isLandscapeCw ? hintGutterWidth : 0;
    const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
    const int contentY = isPortraitInverted ? 50 : 0;
    constexpr int lineHeight = 36;
    const int listTop = 60 + contentY;
    const int visibleCount = std::max(1, (renderer.getScreenHeight() - listTop) / lineHeight);
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTouchDown(tx, ty) && tx >= contentX && tx < contentX + contentWidth && ty >= listTop) {
      const int row = (ty - listTop) / lineHeight;
      const int touched = scrollOffset + row;
      if (row >= 0 && row < visibleCount && touched >= 0 && touched < static_cast<int>(footnotes.size())) {
        if (selectedIndex != touched) {
          selectedIndex = touched;
          requestUpdate();
        }
        return;
      }
    }

    if (mappedInput.wasScreenTapped(tx, ty) && tx >= contentX && tx < contentX + contentWidth && ty >= listTop) {
      const int row = (ty - listTop) / lineHeight;
      const int tapped = scrollOffset + row;
      if (row >= 0 && row < visibleCount && tapped >= 0 && tapped < static_cast<int>(footnotes.size())) {
        selectedIndex = tapped;
        selectFootnote();
        return;
      }
    }

    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up) {
      selectedIndex = std::min(static_cast<int>(footnotes.size()) - 1, selectedIndex + visibleCount);
      requestUpdate();
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Down) {
      selectedIndex = std::max(0, selectedIndex - visibleCount);
      requestUpdate();
      return;
    }
  }

  buttonNavigator.onNext([this] {
    if (!footnotes.empty()) {
      selectedIndex = (selectedIndex + 1) % footnotes.size();
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (!footnotes.empty()) {
      selectedIndex = (selectedIndex - 1 + footnotes.size()) % footnotes.size();
      requestUpdate();
    }
  });
}

void EpubReaderFootnotesActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  // Landscape orientation: reserve a horizontal gutter for button hints.
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Inverted portrait: reserve vertical space for hints at the top.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  // Manual centering to honor content gutters.
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_FOOTNOTES), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_FOOTNOTES), true, EpdFontFamily::BOLD);

  if (footnotes.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 90 + contentY, tr(STR_NO_FOOTNOTES));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  constexpr int lineHeight = 36;
  const int screenWidth = renderer.getScreenWidth();
  const int marginLeft = contentX + 20;
  const int listTop = 60 + contentY;

  const int visibleCount = std::max(1, (renderer.getScreenHeight() - listTop) / lineHeight);
  if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
  if (selectedIndex >= scrollOffset + visibleCount) scrollOffset = selectedIndex - visibleCount + 1;

  for (int i = scrollOffset; i < static_cast<int>(footnotes.size()) && i < scrollOffset + visibleCount; i++) {
    const int y = listTop + (i - scrollOffset) * lineHeight;
    const bool isSelected = (i == selectedIndex);

    if (isSelected) {
      renderer.fillRect(0, y, screenWidth, lineHeight, true);
    }

    // Show footnote number and abbreviated href
    std::string label = footnotes[i].number;
    if (label.empty()) {
      label = tr(STR_LINK);
    }
    renderer.drawText(UI_10_FONT_ID, marginLeft, y + 4, label.c_str(), !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
