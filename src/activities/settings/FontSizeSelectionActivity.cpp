#include "FontSizeSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <cstdio>

#include "I18n.h"

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* ELLIPSIS_UTF8 = "\xe2\x80\xa6";

int findCurrentFontSizeIndex(uint8_t fontSize) {
  // Font size is a simple enum: SMALL=0, MEDIUM=1, LARGE=2, EXTRA_LARGE=3
  return fontSize < CrossPointSettings::FONT_SIZE_COUNT ? fontSize : 1;  // Default to MEDIUM
}
}  // namespace

FontSizeSelectionActivity::FontSizeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FontSizeSelect", renderer, mappedInput) {}

void FontSizeSelectionActivity::onEnter() {
  Activity::onEnter();

  // Get metrics and calculate layout dimensions
  metrics_ = UITheme::getInstance().getMetrics();
  afterHeader = metrics_.topPadding + metrics_.headerHeight + metrics_.verticalSpacing;
  bottomReserved = metrics_.buttonHintsHeight + metrics_.verticalSpacing;
  usableHeight = renderer.getScreenHeight() - afterHeader - bottomReserved;
  previewHeight = usableHeight * metrics_.previewHeightPercent / 100;

  // Save original font size setting
  originalFontSize_ = SETTINGS.fontSize;

  // Build font size list
  fontSizes_.clear();
  fontSizes_.reserve(CrossPointSettings::FONT_SIZE_COUNT);

  fontSizes_.push_back({I18N.get(StrId::STR_SMALL), static_cast<uint8_t>(CrossPointSettings::SMALL)});
  fontSizes_.push_back({I18N.get(StrId::STR_MEDIUM), static_cast<uint8_t>(CrossPointSettings::MEDIUM)});
  fontSizes_.push_back({I18N.get(StrId::STR_LARGE), static_cast<uint8_t>(CrossPointSettings::LARGE)});
  fontSizes_.push_back({I18N.get(StrId::STR_X_LARGE), static_cast<uint8_t>(CrossPointSettings::EXTRA_LARGE)});

  selectedIndex_ = findCurrentFontSizeIndex(SETTINGS.fontSize);
  previewFontSizeIndex_ = selectedIndex_;

  requestUpdate();
}

void FontSizeSelectionActivity::onExit() { Activity::onExit(); }

void FontSizeSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // Restore original font size setting
    SETTINGS.fontSize = originalFontSize_;
    sdFontSystem.ensureLoaded(renderer);
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex_ == previewFontSizeIndex_) {
      handleSelection();
    } else {
      // Preview the selected font size
      previewFontSizeIndex_ = selectedIndex_;
      SETTINGS.fontSize = fontSizes_[selectedIndex_].settingIndex;
      sdFontSystem.ensureLoaded(renderer);
      requestUpdate();
    }
    return;
  }

  const int listSize = static_cast<int>(fontSizes_.size());
  const int pageItems =
      UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, previewHeight + metrics_.verticalSpacing);

  buttonNavigator_.onNextRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onPreviousRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });
}

void FontSizeSelectionActivity::handleSelection() {
  SETTINGS.fontSize = fontSizes_[selectedIndex_].settingIndex;
  sdFontSystem.ensureLoaded(renderer);
  finish();
}

void FontSizeSelectionActivity::renderPreviewPane(int top, int height, int fontId, const char* sizeName) const {
  const int left = metrics_.previewPadding;
  const int width = renderer.getScreenWidth() - (metrics_.previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  const int labelFontId = UI_10_FONT_ID;
  const int labelH = renderer.getTextHeight(labelFontId);
  const int labelGap = 4;
  const int labelReserved = labelH + labelGap + metrics_.previewPadding;

  char labelBuf[128];
  snprintf(labelBuf, sizeof(labelBuf), "%s \"%s\"", tr(STR_PREVIEW), sizeName ? sizeName : "");
  const int labelY = top + height - metrics_.previewPadding - labelH;
  renderer.drawText(labelFontId, left, labelY, labelBuf);

  if (fontId == 0) return;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const int innerHeight = height - metrics_.previewPadding - labelReserved;
  const int maxLines = std::max(1, innerHeight / (lineH + 2));

  const char* previewText = I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
  if (auto* fcm = renderer.getFontCacheManager()) {
    char prewarmBuf[256];
    snprintf(prewarmBuf, sizeof(prewarmBuf), "%s %s", previewText, ELLIPSIS_UTF8);
    fcm->prewarmCache(fontId, prewarmBuf, 0x01);
  }

  const auto lines = renderer.wrappedText(fontId, previewText, width, maxLines);

  int y = top + metrics_.previewPadding;
  const int textBottomLimit = top + height - labelReserved;
  for (const auto& line : lines) {
    if (y + lineH > textBottomLimit) break;
    renderer.drawText(fontId, left, y, line.c_str());
    y += lineH + 2;
  }
}

void FontSizeSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics_.topPadding, pageWidth, metrics_.headerHeight}, tr(STR_FONT_SIZE));

  const int previewTop = afterHeader;
  const int listTop = previewTop + previewHeight + metrics_.verticalSpacing;
  const int listHeight = usableHeight - previewHeight - metrics_.verticalSpacing;

  // Get the font ID for the preview (using current font family and previewed size)
  const int previewFontId = SETTINGS.getReaderFontId();
  const char* previewSizeName = (previewFontSizeIndex_ >= 0 && previewFontSizeIndex_ < static_cast<int>(fontSizes_.size()))
                                   ? fontSizes_[previewFontSizeIndex_].name.c_str()
                                   : nullptr;
  renderPreviewPane(previewTop, previewHeight, previewFontId, previewSizeName);

  renderer.drawLine(0, listTop - metrics_.verticalSpacing / 2, pageWidth, listTop - metrics_.verticalSpacing / 2);

  const int currentFontSizeIndex = findCurrentFontSizeIndex(originalFontSize_);
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, static_cast<int>(fontSizes_.size()), selectedIndex_,
      [this](int index) { return fontSizes_[index].name; }, nullptr, nullptr,
      [this, currentFontSizeIndex](int index) -> std::string {
        if (index == previewFontSizeIndex_ && index != currentFontSizeIndex) return tr(STR_PREVIEW);
        if (index == currentFontSizeIndex) return tr(STR_SELECTED);
        return "";
      },
      true);

  const bool onPreviewed = selectedIndex_ == previewFontSizeIndex_;
  const char* confirmLabel = onPreviewed ? tr(STR_SELECT) : tr(STR_PREVIEW);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
