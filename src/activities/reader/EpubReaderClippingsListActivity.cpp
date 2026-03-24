#include "EpubReaderClippingsListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <string>

#include "ClippingTextViewerActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
const int LINE_HEIGHT = 60;
}  // namespace

void EpubReaderClippingsListActivity::refreshPreviews() {
  previewCache.clear();
  previewCache.reserve(clippings.size());
  for (const auto& entry : clippings) {
    previewCache.push_back(ClippingStore::loadClippingPreview(bookPath, entry, 200));
  }
}

void EpubReaderClippingsListActivity::onEnter() {
  Activity::onEnter();

  clippings = ClippingStore::loadIndex(bookPath);
  refreshPreviews();

  if (selectorIndex >= static_cast<int>(clippings.size())) {
    selectorIndex = std::max(0, static_cast<int>(clippings.size()) - 1);
  }

  requestUpdate();
}

void EpubReaderClippingsListActivity::onExit() { Activity::onExit(); }

void EpubReaderClippingsListActivity::loop() {
  if (confirmingDelete) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!ClippingStore::deleteClipping(bookPath, selectorIndex)) {
        LOG_ERR("ClippingsList", "Failed to delete clipping at index %d", selectorIndex);
      } else {
        clippings = ClippingStore::loadIndex(bookPath);
        refreshPreviews();
        if (selectorIndex >= static_cast<int>(clippings.size()) && selectorIndex > 0) {
          selectorIndex--;
        }
      }
      requestUpdate();
      confirmingDelete = false;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      requestUpdate();
      confirmingDelete = false;
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() > SKIP_PAGE_MS) {
      confirmingDelete = true;
      requestUpdate();
    } else if (!clippings.empty()) {
      const std::string text = ClippingStore::loadClippingText(bookPath, clippings[static_cast<size_t>(selectorIndex)]);
      if (!text.empty()) {
        startActivityForResult(std::make_unique<ClippingTextViewerActivity>(renderer, mappedInput, text),
                               [this](const ActivityResult&) { requestUpdate(); });
      }
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }

  buttonNavigator.onNextRelease([this] {
    if (!clippings.empty()) {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, static_cast<int>(clippings.size()));
      requestUpdate();
    }
  });

  buttonNavigator.onPreviousRelease([this] {
    if (!clippings.empty()) {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, static_cast<int>(clippings.size()));
      requestUpdate();
    }
  });
}

void EpubReaderClippingsListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const bool isPortrait = orientation == GfxRenderer::Orientation::Portrait;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 40 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int hintGutterBottom = isPortrait ? 75 : 40;
  const int contentY = hintGutterHeight;
  const int listY = contentY + LINE_HEIGHT;
  const int listHeight = pageHeight - hintGutterBottom - LINE_HEIGHT;
  const int numItems = static_cast<int>(clippings.size());

  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_CLIPPINGS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_CLIPPINGS), true, EpdFontFamily::BOLD);

  const auto getRowTitle = [this](int index) {
    return previewCache.at(static_cast<size_t>(confirmingDelete ? selectorIndex : index));
  };

  const auto getRowSubtitle = [this](int index) {
    const auto& e = clippings.at(static_cast<size_t>(confirmingDelete ? selectorIndex : index));
    const std::string pages = "p." + std::to_string(static_cast<unsigned>(e.startPage) + 1U) + "-" +
                              std::to_string(static_cast<unsigned>(e.endPage) + 1U);
    if (!epub) {
      return std::to_string(static_cast<unsigned>(e.bookPercent)) + "% · " + pages;
    }
    const int tocIndex = epub->getTocIndexForSpineIndex(e.spineIndex);
    const std::string tocTitle = (tocIndex >= 0) ? epub->getTocItem(tocIndex).title : std::string(tr(STR_UNNAMED));
    return std::to_string(static_cast<unsigned>(e.bookPercent)) + "% · " + tocTitle + " · " + pages;
  };

  const auto getClippingIcon = [isPortrait](int index) {
    (void)index;
    return isPortrait ? UIIcon::Bookmark : UIIcon::None;
  };

  if (numItems > 0) {
    if (confirmingDelete) {
      GUI.drawHelpText(renderer, Rect{0, pageHeight / 2 - LINE_HEIGHT * 2, contentWidth, LINE_HEIGHT},
                       tr(STR_DELETE_CLIPPING_CONFIRM));

      GUI.drawList(renderer, Rect{contentX, pageHeight / 2, contentWidth, LINE_HEIGHT}, 1, 0, getRowTitle,
                   getRowSubtitle, getClippingIcon);
    } else {
      GUI.drawList(renderer, Rect{contentX, listY, contentWidth, listHeight}, numItems, selectorIndex, getRowTitle,
                   getRowSubtitle, getClippingIcon);

      GUI.drawHelpText(renderer, Rect{contentX, pageHeight - hintGutterBottom, contentWidth, LINE_HEIGHT},
                       tr(STR_HOLD_CONFIRM_TO_DELETE));
    }
  } else {
    GUI.drawHelpText(renderer, Rect{contentX, LINE_HEIGHT * 2, contentWidth, LINE_HEIGHT},
                     tr(STR_CLIPPINGS_INSTRUCTIONS));
    GUI.drawHelpText(renderer, Rect{contentX, LINE_HEIGHT * 4, contentWidth, LINE_HEIGHT}, tr(STR_NO_CLIPPINGS));
  }

  const auto backLabel = confirmingDelete ? tr(STR_CANCEL) : tr(STR_BACK);
  const auto confirmLabel = confirmingDelete ? tr(STR_DELETE) : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
