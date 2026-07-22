#include "TextSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "TextSettingsPreview.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Tab labels for Font | Size | Layout | Style (shared by render and loop touch hit-testing).
constexpr StrId TAB_NAME_IDS[] = {StrId::STR_FONT, StrId::STR_SIZE, StrId::STR_LAYOUT, StrId::STR_STYLE};

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
  return fontSize < listSize ? fontSize : 1;  // default MEDIUM
}

constexpr StrId LINE_SPACING_IDS[] = {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE};
constexpr StrId ALIGNMENT_IDS[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                                   StrId::STR_BOOK_S_STYLE};
constexpr int MARGIN_MIN = CrossPointSettings::SCREEN_MARGIN_MIN;
constexpr int MARGIN_MAX = CrossPointSettings::SCREEN_MARGIN_MAX;
constexpr int MARGIN_STEP = CrossPointSettings::SCREEN_MARGIN_STEP;
}  // namespace

TextSettingsActivity::TextSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const SdCardFontRegistry* registry, Tab initialTab)
    : Activity("TextSettings", renderer, mappedInput), registry_(registry), tab_(initialTab) {}

void TextSettingsActivity::onEnter() {
  Activity::onEnter();

  metrics_ = UITheme::getInstance().getMetrics();
  afterHeader = metrics_.topPadding + metrics_.headerHeight + metrics_.verticalSpacing;
  bottomReserved = metrics_.buttonHintsHeight + metrics_.verticalSpacing;
  usableHeight = renderer.getScreenHeight() - afterHeader - bottomReserved;
  previewHeight = usableHeight * metrics_.previewHeightPercent / 100;

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

  sizes_.clear();
  sizes_.reserve(CrossPointSettings::FONT_SIZE_COUNT);
  sizes_.push_back({I18N.get(StrId::STR_SMALL), static_cast<uint8_t>(CrossPointSettings::SMALL)});
  sizes_.push_back({I18N.get(StrId::STR_MEDIUM), static_cast<uint8_t>(CrossPointSettings::MEDIUM)});
  sizes_.push_back({I18N.get(StrId::STR_LARGE), static_cast<uint8_t>(CrossPointSettings::LARGE)});
  sizes_.push_back({I18N.get(StrId::STR_X_LARGE), static_cast<uint8_t>(CrossPointSettings::EXTRA_LARGE)});

  currentFamilyIndex_ = findCurrentFontIndex(registry_, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily);
  currentSizeIndex_ = findCurrentFontSizeIndex(SETTINGS.fontSize, sizes_.size());
  std::fill(std::begin(selectedIndex_), std::end(selectedIndex_), 1);       // default to the first list row
  selectedIndex_[static_cast<int>(Tab::Family)] = currentFamilyIndex_ + 1;  // Family/Size open on current selection
  selectedIndex_[static_cast<int>(Tab::Size)] = currentSizeIndex_ + 1;

  requestUpdate();
}

void TextSettingsActivity::onExit() { Activity::onExit(); }

TextSettingsActivity::PaneGeometry TextSettingsActivity::paneGeometry() const {
  const int previewTop = afterHeader;
  const int tabTop = previewTop + previewHeight;
  const int captionH = renderer.getTextHeight(UI_10_FONT_ID) + metrics_.verticalSpacing;
  const int listTop = tabTop + metrics_.tabBarHeight + metrics_.verticalSpacing;
  const int listHeight = usableHeight - previewHeight - metrics_.tabBarHeight - metrics_.verticalSpacing - captionH;
  return {previewTop, tabTop, listTop, listHeight};
}

bool TextSettingsActivity::handleTouch() {
  // Inert on non-touch boards: the events simply never fire.
  int tx = 0;
  int ty = 0;
  const auto geo = paneGeometry();
  const int listCount = currentListSize();

  // TODO: this tab-bar touch pass duplicates SettingsActivity's
  // this will have to be refactored when a handleTabBarTouch() helper exist
  // (similar to handleListTouch)
  auto buildTabs = [this]() {
    std::vector<TabInfo> tabs;
    tabs.reserve(static_cast<int>(Tab::Count));
    for (int t = 0; t < static_cast<int>(Tab::Count); t++) {
      tabs.push_back({I18N.get(TAB_NAME_IDS[t]), tab_ == static_cast<Tab>(t)});
    }
    return tabs;
  };
  int tabHit = -1;
  if ((mappedInput.wasScreenTouchDown(tx, ty) || mappedInput.wasScreenTapped(tx, ty)) &&
      GUI.tabIndexFromPoint(renderer, Rect{0, geo.tabTop, renderer.getScreenWidth(), metrics_.tabBarHeight},
                            buildTabs(), tx, ty, tabHit)) {
    if (tab_ != static_cast<Tab>(tabHit)) {
      tab_ = static_cast<Tab>(tabHit);
      selectedIndex() = 0;
      requestUpdate();
    }
    return true;
  }

  int row = std::max(0, selectedIndex() - 1);
  switch (handleListTouch(row, listCount, geo.listTop, geo.listHeight, /*hasSubtitle=*/false)) {
    case ListTouchResult::Activated:
      selectedIndex() = row + 1;
      activateRow(row);
      return true;
    case ListTouchResult::Consumed:
      selectedIndex() = row + 1;
      return true;
    case ListTouchResult::None:
      break;
  }

  // Vertical swipe pages the list (Family/Size); short lists just clamp.
  const int pageItems = GUI.getListPageItems(geo.listHeight, false);
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectedIndex() =
        selectedIndex() == 0 ? 1 : ButtonNavigator::nextPageIndex(selectedIndex(), listCount + 1, pageItems);
    requestUpdate();
    return true;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectedIndex() = ButtonNavigator::previousPageIndex(selectedIndex(), listCount + 1, pageItems);
    requestUpdate();
    return true;
  }

  return false;
}

void TextSettingsActivity::loop() {
  if (optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) return;  // picker owns input while open

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex() == 0) {
      switchTab();
      return;
    }

    activateRow(selectedIndex() - 1);
    return;
  }

  if (handleTouch()) return;

  const int ringSize = currentListSize() + 1;  // +1 for the tab bar at position 0

  buttonNavigator_.onNextRelease([this, ringSize] {
    selectedIndex() = ButtonNavigator::nextIndex(selectedIndex(), ringSize);
    requestUpdate();
  });

  buttonNavigator_.onPreviousRelease([this, ringSize] {
    selectedIndex() = ButtonNavigator::previousIndex(selectedIndex(), ringSize);
    requestUpdate();
  });

  buttonNavigator_.onNextContinuous([this] { switchTab(); });
  buttonNavigator_.onPreviousContinuous([this] { switchTab(-1); });
}

void TextSettingsActivity::render(RenderLock&&) {
  if (optionPopup_.processRender(renderer, mappedInput)) return;  // picker draws over everything

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics_.topPadding, pageWidth, metrics_.headerHeight}, tr(STR_TEXT_SETTINGS));

  const auto geo = paneGeometry();
  const char* familyName = (currentFamilyIndex_ >= 0 && currentFamilyIndex_ < static_cast<int>(fonts_.size()))
                               ? fonts_[currentFamilyIndex_].name.c_str()
                               : "";
  const char* sizeName = (currentSizeIndex_ >= 0 && currentSizeIndex_ < static_cast<int>(sizes_.size()))
                             ? sizes_[currentSizeIndex_].name.c_str()
                             : "";
  textsettings::renderPreview(renderer, previewLayout_, metrics_.previewPadding, metrics_.verticalSpacing,
                              geo.previewTop, previewHeight, familyName, sizeName);

  const bool onTabBar = selectedIndex() == 0;
  std::vector<TabInfo> tabs;
  tabs.reserve(static_cast<int>(Tab::Count));
  for (int t = 0; t < static_cast<int>(Tab::Count); t++) {
    tabs.push_back({I18N.get(TAB_NAME_IDS[t]), tab_ == static_cast<Tab>(t)});
  }
  GUI.drawTabBar(renderer, Rect{0, geo.tabTop, pageWidth, metrics_.tabBarHeight}, tabs, onTabBar);

  const Rect listRect{0, geo.listTop, pageWidth, geo.listHeight};
  const int selectedItem = selectedIndex() - 1;
  const char* confirmLabel = tr(STR_SELECT);

  switch (tab_) {
    case Tab::Family:
      GUI.drawList(
          renderer, listRect, static_cast<int>(fonts_.size()), selectedItem,
          [this](int index) { return fonts_[index].name; }, nullptr, nullptr,
          [this](int index) -> std::string { return index == currentFamilyIndex_ ? tr(STR_SELECTED) : ""; }, true);
      if (onTabBar) confirmLabel = tr(STR_SIZE);
      break;

    case Tab::Size:
      GUI.drawList(
          renderer, listRect, static_cast<int>(sizes_.size()), selectedItem,
          [this](int index) { return sizes_[index].name; }, nullptr, nullptr,
          [this](int index) -> std::string { return index == currentSizeIndex_ ? tr(STR_SELECTED) : ""; }, true);
      if (onTabBar) confirmLabel = tr(STR_LAYOUT);
      break;

    case Tab::Layout: {
      constexpr int LAYOUT_ROWS = static_cast<int>(LayoutRow::Count);
      static constexpr StrId ROW_NAME_IDS[LAYOUT_ROWS] = {StrId::STR_LINE_SPACING, StrId::STR_EXTRA_SPACING,
                                                          StrId::STR_ALIGNMENT, StrId::STR_SCREEN_MARGIN};
      GUI.drawList(
          renderer, listRect, LAYOUT_ROWS, selectedItem,
          [](int index) { return std::string(I18N.get(ROW_NAME_IDS[index])); }, nullptr, nullptr,
          [this](int index) { return layoutValueText(index); }, true);
      if (onTabBar)
        confirmLabel = tr(STR_STYLE);
      else  // Extra Paragraph Spacing toggles; the rest open a picker
        confirmLabel = (selectedItem == static_cast<int>(LayoutRow::ParaSpacing)) ? tr(STR_TOGGLE) : tr(STR_SELECT);
      break;
    }

    case Tab::Style: {
      constexpr int STYLE_ROWS = static_cast<int>(StyleRow::Count);
      static constexpr StrId ROW_NAME_IDS[STYLE_ROWS] = {StrId::STR_FOCUS_READING, StrId::STR_HYPHENATION,
                                                         StrId::STR_EMBEDDED_STYLE, StrId::STR_TEXT_AA};
      GUI.drawList(
          renderer, listRect, STYLE_ROWS, selectedItem,
          [](int index) { return std::string(I18N.get(ROW_NAME_IDS[index])); }, nullptr, nullptr,
          [this](int index) { return styleValueText(index); }, true);
      confirmLabel = onTabBar ? tr(STR_FONT) : tr(STR_TOGGLE);
      break;
    }

    default:
      break;
  }

  if (focusedRowHasNoPreview()) {
    const int capY = geo.listTop + geo.listHeight + metrics_.verticalSpacing;
    renderer.drawText(UI_10_FONT_ID, metrics_.previewPadding, capY, tr(STR_NOT_IN_PREVIEW));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

// Font switching runs on the main task from loop(), which deliberately holds no
// RenderLock. ensureLoaded() deletes the resident SdCardFont before loading the
// next one, and the render task walks that same object inside the preview's
// prewarmCache() — so without this lock a font switch can free the mini glyph
// arrays out from under prewarmStyle() (crash: null s.miniGlyphs mid-read/sort).
void TextSettingsActivity::applyFamily(int listIndex) {
  RenderLock lock;
  const auto& font = fonts_[listIndex];
  if (font.isBuiltin) {
    SETTINGS.fontFamily = font.settingIndex;
    SETTINGS.sdFontFamilyName[0] = '\0';
    sdFontSystem.ensureLoaded(renderer);  // unloads the previously resident SD font
    currentFamilyIndex_ = listIndex;
  } else if (registry_) {
    const int sdIdx = font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIdx < static_cast<int>(families.size())) {
      strncpy(SETTINGS.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      sdFontSystem.ensureLoaded(renderer);
      currentFamilyIndex_ = listIndex;
    }
  }
}

void TextSettingsActivity::activateRow(int row) {
  switch (tab_) {
    case Tab::Family:
      if (row != currentFamilyIndex_) {
        applyFamily(row);
        requestUpdate();
      }
      break;
    case Tab::Size:
      if (row != currentSizeIndex_) {
        applySize(row);
        requestUpdate();
      }
      break;
    case Tab::Layout:
      confirmLayoutRow(row);
      break;
    case Tab::Style:
      confirmStyleRow(row);
      break;
    default:
      break;
  }
}

// Same RenderLock rationale as applyFamily(): a size change reloads the SD font
// file, which frees and replaces the SdCardFont the render task may be reading.
void TextSettingsActivity::applySize(int listIndex) {
  RenderLock lock;

  currentSizeIndex_ = listIndex;
  SETTINGS.fontSize = sizes_[listIndex].settingIndex;
  sdFontSystem.ensureLoaded(renderer);
}

void TextSettingsActivity::confirmLayoutRow(int row) {
  switch (static_cast<LayoutRow>(row)) {
    case LayoutRow::ParaSpacing:
      SETTINGS.extraParagraphSpacing = !SETTINGS.extraParagraphSpacing;
      requestUpdate();
      break;
    case LayoutRow::LineSpacing:
      optionPopup_.show(StrId::STR_LINE_SPACING, LINE_SPACING_IDS, static_cast<int>(std::size(LINE_SPACING_IDS)),
                        SETTINGS.lineSpacing, [](int idx) { SETTINGS.lineSpacing = static_cast<uint8_t>(idx); });
      requestUpdate();
      break;
    case LayoutRow::Alignment:
      optionPopup_.show(StrId::STR_ALIGNMENT, ALIGNMENT_IDS, static_cast<int>(std::size(ALIGNMENT_IDS)),
                        SETTINGS.paragraphAlignment,
                        [](int idx) { SETTINGS.paragraphAlignment = static_cast<uint8_t>(idx); });
      requestUpdate();
      break;
    case LayoutRow::ScreenMargin: {
      std::vector<std::string> options;
      options.reserve((MARGIN_MAX - MARGIN_MIN) / MARGIN_STEP + 1);
      for (int m = MARGIN_MIN; m <= MARGIN_MAX; m += MARGIN_STEP) options.push_back(std::to_string(m));
      const int cur = (std::clamp<int>(SETTINGS.screenMargin, MARGIN_MIN, MARGIN_MAX) - MARGIN_MIN) / MARGIN_STEP;
      optionPopup_.show(StrId::STR_SCREEN_MARGIN, options, cur,
                        [](int idx) { SETTINGS.screenMargin = static_cast<uint8_t>(MARGIN_MIN + idx * MARGIN_STEP); });
      requestUpdate();
      break;
    }

    default:
      break;
  }
}

std::string TextSettingsActivity::layoutValueText(int row) const {
  switch (static_cast<LayoutRow>(row)) {
    case LayoutRow::LineSpacing: {
      const uint8_t v = SETTINGS.lineSpacing;
      return v < std::size(LINE_SPACING_IDS) ? I18N.get(LINE_SPACING_IDS[v]) : I18N.get(StrId::STR_NORMAL);
    }
    case LayoutRow::ParaSpacing:
      return SETTINGS.extraParagraphSpacing ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case LayoutRow::Alignment: {
      const uint8_t v = SETTINGS.paragraphAlignment;
      return v < std::size(ALIGNMENT_IDS) ? I18N.get(ALIGNMENT_IDS[v]) : I18N.get(StrId::STR_JUSTIFY);
    }
    case LayoutRow::ScreenMargin:
      return std::to_string(SETTINGS.screenMargin);

    default:
      return "";
  }
}

void TextSettingsActivity::confirmStyleRow(int row) {
  switch (static_cast<StyleRow>(row)) {
    case StyleRow::FocusReading:
      SETTINGS.focusReadingEnabled = !SETTINGS.focusReadingEnabled;
      break;
    case StyleRow::Hyphenation:
      SETTINGS.hyphenationEnabled = !SETTINGS.hyphenationEnabled;
      break;
    case StyleRow::EmbeddedStyle:
      SETTINGS.embeddedStyle = !SETTINGS.embeddedStyle;
      break;
    case StyleRow::AntiAliasing:
      SETTINGS.textAntiAliasing = !SETTINGS.textAntiAliasing;
      break;

    default:
      return;
  }
  requestUpdate();
}

std::string TextSettingsActivity::styleValueText(int row) const {
  switch (static_cast<StyleRow>(row)) {
    case StyleRow::FocusReading:
      return SETTINGS.focusReadingEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case StyleRow::Hyphenation:
      return SETTINGS.hyphenationEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case StyleRow::EmbeddedStyle:
      return SETTINGS.embeddedStyle ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case StyleRow::AntiAliasing:
      return SETTINGS.textAntiAliasing ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);

    default:
      return "";
  }
}

// Only Focus Reading shows in the preview (bold prefixes); the other Style rows
// have no distinct preview.
bool TextSettingsActivity::focusedRowHasNoPreview() const {
  if (selectedIndex() == 0 || tab_ != Tab::Style) return false;
  const StyleRow row = static_cast<StyleRow>(selectedIndex() - 1);
  return row == StyleRow::Hyphenation || row == StyleRow::EmbeddedStyle || row == StyleRow::AntiAliasing;
}

void TextSettingsActivity::switchTab(int direction) {
  const bool onTabBar = selectedIndex() == 0;
  constexpr int tabCount = static_cast<int>(Tab::Count);
  tab_ = static_cast<Tab>((static_cast<int>(tab_) + direction + tabCount) % tabCount);
  if (onTabBar) selectedIndex() = 0;
  requestUpdate();
}

int TextSettingsActivity::currentListSize() const {
  switch (tab_) {
    case Tab::Family:
      return static_cast<int>(fonts_.size());
    case Tab::Size:
      return static_cast<int>(sizes_.size());
    case Tab::Layout:
      return static_cast<int>(LayoutRow::Count);
    case Tab::Style:
      return static_cast<int>(StyleRow::Count);

    default:
      return 0;
  }
}

int& TextSettingsActivity::selectedIndex() { return selectedIndex_[static_cast<int>(tab_)]; }
int TextSettingsActivity::selectedIndex() const { return selectedIndex_[static_cast<int>(tab_)]; }
