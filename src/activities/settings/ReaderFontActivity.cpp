#include "ReaderFontActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* ELLIPSIS_UTF8 = "\xe2\x80\xa6";

int findCurrentFontIndex(const SdCardFontRegistry* registry, const char* sdFontFamilyName, uint8_t fontFamily) {
  if (sdFontFamilyName[0] != '\0' && registry) {
    const auto& families = registry->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      if (families[i].name == sdFontFamilyName) {
        return CrossPointSettings::BUILTIN_FONT_COUNT + i;
      }
    }
  }

  return fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? fontFamily : 0;
}

int findCurrentFontSizeIndex(uint8_t fontSize, size_t listSize) {
  // Font size is a simple enum: SMALL=0, MEDIUM=1, LARGE=2, EXTRA_LARGE=3
  return fontSize < listSize ? fontSize : 1;  // Default to MEDIUM
}
}  // namespace

ReaderFontActivity::ReaderFontActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const SdCardFontRegistry* registry, Tab initialTab)
    : Activity("ReaderFont", renderer, mappedInput), registry_(registry), tab_(initialTab) {}

void ReaderFontActivity::onEnter() {
  Activity::onEnter();

  // Get metrics and calculate layout dimensions
  metrics_ = UITheme::getInstance().getMetrics();
  afterHeader = metrics_.topPadding + metrics_.headerHeight + metrics_.verticalSpacing;
  bottomReserved = metrics_.buttonHintsHeight + metrics_.verticalSpacing;
  usableHeight = renderer.getScreenHeight() - afterHeader - bottomReserved;
  previewHeight = usableHeight * metrics_.previewHeightPercent / 100;

  // Save original settings so Back can revert both dimensions
  originalFontFamily_ = SETTINGS.fontFamily;
  originalFontSize_ = SETTINGS.fontSize;
  strncpy(originalSdFontFamilyName_, SETTINGS.sdFontFamilyName, sizeof(originalSdFontFamilyName_) - 1);
  originalSdFontFamilyName_[sizeof(originalSdFontFamilyName_) - 1] = '\0';

  // Build font family list
  fonts_.clear();
  fonts_.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + (registry_ ? registry_->getFamilyCount() : 0));

  fonts_.push_back({I18N.get(StrId::STR_NOTO_SERIF), true, static_cast<uint8_t>(CrossPointSettings::NOTOSERIF)});
  fonts_.push_back({I18N.get(StrId::STR_NOTO_SANS), true, static_cast<uint8_t>(CrossPointSettings::NOTOSANS)});

  if (registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      fonts_.push_back({families[i].name, false, static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i)});
    }
  }

  // Build font size list
  sizes_.clear();
  sizes_.reserve(CrossPointSettings::FONT_SIZE_COUNT);

  sizes_.push_back({I18N.get(StrId::STR_SMALL), static_cast<uint8_t>(CrossPointSettings::SMALL)});
  sizes_.push_back({I18N.get(StrId::STR_MEDIUM), static_cast<uint8_t>(CrossPointSettings::MEDIUM)});
  sizes_.push_back({I18N.get(StrId::STR_LARGE), static_cast<uint8_t>(CrossPointSettings::LARGE)});
  sizes_.push_back({I18N.get(StrId::STR_X_LARGE), static_cast<uint8_t>(CrossPointSettings::EXTRA_LARGE)});

  currentFamilyIndex_ = findCurrentFontIndex(registry_, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily);
  currentSizeIndex_ = findCurrentFontSizeIndex(SETTINGS.fontSize, sizes_.size());
  previewFamilyIndex_ = currentFamilyIndex_;
  previewSizeIndex_ = currentSizeIndex_;
  navFamily_ = currentFamilyIndex_ + 1;
  navSize_ = currentSizeIndex_ + 1;

  requestUpdate();
}

void ReaderFontActivity::onExit() { Activity::onExit(); }

int ReaderFontActivity::currentListSize() const {
  return static_cast<int>(tab_ == Tab::Family ? fonts_.size() : sizes_.size());
}

int& ReaderFontActivity::navIndex() { return tab_ == Tab::Family ? navFamily_ : navSize_; }

int ReaderFontActivity::navIndex() const { return tab_ == Tab::Family ? navFamily_ : navSize_; }

void ReaderFontActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    revertAndExit();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (navIndex() == 0) {
      switchTab();
      return;
    }

    const int itemIndex = navIndex() - 1;
    if (tab_ == Tab::Family) {
      if (itemIndex == previewFamilyIndex_) {
        // Already previewed: apply the pending family + size combination
        finish();
        return;
      }
      previewFamily(itemIndex);
    } else {
      if (itemIndex == previewSizeIndex_) {
        finish();
        return;
      }
      previewSize(itemIndex);
    }
    requestUpdate();
    return;
  }

  // Ring size includes the tab bar at position 0
  const int ringSize = currentListSize() + 1;

  buttonNavigator_.onNextRelease([this, ringSize] {
    navIndex() = ButtonNavigator::nextIndex(navIndex(), ringSize);
    requestUpdate();
  });

  buttonNavigator_.onPreviousRelease([this, ringSize] {
    navIndex() = ButtonNavigator::previousIndex(navIndex(), ringSize);
    requestUpdate();
  });

  // Hold switches tab, same as category switching in SettingsActivity
  buttonNavigator_.onNextContinuous([this] { switchTab(); });
  buttonNavigator_.onPreviousContinuous([this] { switchTab(); });
}

void ReaderFontActivity::previewFamily(int listIndex) {
  previewFamilyIndex_ = listIndex;
  const auto& font = fonts_[listIndex];
  if (font.isBuiltin) {
    SETTINGS.fontFamily = font.settingIndex;
    SETTINGS.sdFontFamilyName[0] = '\0';
  } else if (registry_) {
    const int sdIdx = font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIdx < static_cast<int>(families.size())) {
      strncpy(SETTINGS.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      sdFontSystem.ensureLoaded(renderer);
    }
  }
}

void ReaderFontActivity::switchTab() {
  // Stay on the tab bar if focused there; otherwise land on the first item of
  // the new tab (mirrors SettingsActivity category switching).
  const bool onTabBar = navIndex() == 0;
  tab_ = (tab_ == Tab::Family) ? Tab::Size : Tab::Family;
  navIndex() = onTabBar ? 0 : 1;
  requestUpdate();
}

void ReaderFontActivity::previewSize(int listIndex) {
  previewSizeIndex_ = listIndex;
  SETTINGS.fontSize = sizes_[listIndex].settingIndex;
  sdFontSystem.ensureLoaded(renderer);
}

void ReaderFontActivity::revertAndExit() {
  SETTINGS.fontFamily = originalFontFamily_;
  SETTINGS.fontSize = originalFontSize_;
  strncpy(SETTINGS.sdFontFamilyName, originalSdFontFamilyName_, sizeof(SETTINGS.sdFontFamilyName) - 1);
  SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
  sdFontSystem.ensureLoaded(renderer);
  finish();
}

void ReaderFontActivity::renderPreviewPane(int top, int height) const {
  const int left = metrics_.previewPadding;
  const int width = renderer.getScreenWidth() - (metrics_.previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  const int labelFontId = UI_10_FONT_ID;
  const int labelH = renderer.getTextHeight(labelFontId);
  const int labelGap = 4;
  const int labelReserved = labelH + labelGap + metrics_.previewPadding;

  // Label always shows the full pending combination, e.g. Preview "Noto Sans, Large"
  const char* familyName = (previewFamilyIndex_ >= 0 && previewFamilyIndex_ < static_cast<int>(fonts_.size()))
                               ? fonts_[previewFamilyIndex_].name.c_str()
                               : "";
  const char* sizeName = (previewSizeIndex_ >= 0 && previewSizeIndex_ < static_cast<int>(sizes_.size()))
                             ? sizes_[previewSizeIndex_].name.c_str()
                             : "";
  char labelBuf[128];
  snprintf(labelBuf, sizeof(labelBuf), "%s \"%s, %s\"", tr(STR_PREVIEW), familyName, sizeName);
  const int labelY = top + height - metrics_.previewPadding - labelH;
  renderer.drawText(labelFontId, left, labelY, labelBuf);

  const int fontId = SETTINGS.getReaderFontId();
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

void ReaderFontActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics_.topPadding, pageWidth, metrics_.headerHeight}, tr(STR_READER_FONT));

  const int previewTop = afterHeader;
  renderPreviewPane(previewTop, previewHeight);

  const bool onTabBar = navIndex() == 0;
  const int tabTop = previewTop + previewHeight;
  std::vector<TabInfo> tabs;
  tabs.reserve(2);
  tabs.push_back({tr(STR_FAMILY), tab_ == Tab::Family});
  tabs.push_back({tr(STR_SIZE), tab_ == Tab::Size});
  GUI.drawTabBar(renderer, Rect{0, tabTop, pageWidth, metrics_.tabBarHeight}, tabs, onTabBar);

  const int listTop = tabTop + metrics_.tabBarHeight + metrics_.verticalSpacing;
  const int listHeight = usableHeight - previewHeight - metrics_.tabBarHeight - metrics_.verticalSpacing;
  const Rect listRect{0, listTop, pageWidth, listHeight};
  const int selectedItem = navIndex() - 1;

  if (tab_ == Tab::Family) {
    GUI.drawList(
        renderer, listRect, static_cast<int>(fonts_.size()), selectedItem,
        [this](int index) { return fonts_[index].name; }, nullptr, nullptr,
        [this](int index) -> std::string {
          if (index == previewFamilyIndex_ && index != currentFamilyIndex_) return tr(STR_PREVIEW);
          if (index == currentFamilyIndex_) return tr(STR_SELECTED);
          return "";
        },
        true);
  } else {
    GUI.drawList(
        renderer, listRect, static_cast<int>(sizes_.size()), selectedItem,
        [this](int index) { return sizes_[index].name; }, nullptr, nullptr,
        [this](int index) -> std::string {
          if (index == previewSizeIndex_ && index != currentSizeIndex_) return tr(STR_PREVIEW);
          if (index == currentSizeIndex_) return tr(STR_SELECTED);
          return "";
        },
        true);
  }

  // Confirm hint: on the tab bar it names the tab it switches to; on a
  // previewed item it applies, otherwise it previews.
  const char* confirmLabel;
  if (onTabBar) {
    confirmLabel = (tab_ == Tab::Family) ? tr(STR_SIZE) : tr(STR_FAMILY);
  } else {
    const int previewedIndex = (tab_ == Tab::Family) ? previewFamilyIndex_ : previewSizeIndex_;
    confirmLabel = (selectedItem == previewedIndex) ? tr(STR_SELECT) : tr(STR_PREVIEW);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
