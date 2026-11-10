#include "TextSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "BookTextSettings.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderFontSizes.h"
#include "ReaderLineSpacing.h"
#include "SdCardFontSystem.h"
#include "TextSettingsPreview.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
// Tab labels for Font | Size | Layout | Style.
constexpr StrId TAB_NAME_IDS[] = {StrId::STR_FONT, StrId::STR_SIZE, StrId::STR_LAYOUT, StrId::STR_STYLE};

constexpr StrId LAYOUT_ROW_NAME_IDS[] = {
    StrId::STR_LINE_SPACING,          StrId::STR_WORD_SPACING, StrId::STR_CHARACTER_SPACING, StrId::STR_EXTRA_SPACING,
    StrId::STR_PARAGRAPH_INDENTATION, StrId::STR_ALIGNMENT,    StrId::STR_SCREEN_MARGIN};
constexpr StrId STYLE_ROW_NAME_IDS[] = {StrId::STR_FOCUS_READING, StrId::STR_HYPHENATION_NO_PREVIEW,
                                        StrId::STR_EMBEDDED_STYLE_NO_PREVIEW, StrId::STR_TEXT_AA_NO_PREVIEW,
                                        StrId::STR_USE_DEFAULTS};

int findCurrentFontIndex(const SdCardFontRegistry* registry, const char* sdFontFamilyName) {
  if (sdFontFamilyName[0] != '\0' && registry) {
    const auto& families = registry->getFamilies();
    const auto family = std::find_if(families.begin(), families.end(), [sdFontFamilyName](const auto& candidate) {
      return candidate.name == sdFontFamilyName;
    });
    if (family != families.end()) {
      return CrossPointSettings::BUILTIN_FONT_COUNT + static_cast<int>(family - families.begin());
    }
  }

  return sdFontFamilyName[0] == '\0' ? 0 : -1;
}

constexpr StrId WORD_SPACING_IDS[] = {StrId::STR_SPACING_50_PERCENT,  StrId::STR_SPACING_75_PERCENT,
                                      StrId::STR_SPACING_100_PERCENT, StrId::STR_SPACING_125_PERCENT,
                                      StrId::STR_SPACING_150_PERCENT, StrId::STR_SPACING_175_PERCENT,
                                      StrId::STR_SPACING_200_PERCENT};
constexpr StrId CHARACTER_SPACING_IDS[] = {StrId::STR_SPACING_MINUS_2, StrId::STR_SPACING_MINUS_1,
                                           StrId::STR_SPACING_ZERO, StrId::STR_SPACING_PLUS_1,
                                           StrId::STR_SPACING_PLUS_2};
constexpr StrId ALIGNMENT_IDS[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                                   StrId::STR_BOOK_S_STYLE};
constexpr int MARGIN_MIN = CrossPointSettings::SCREEN_MARGIN_MIN;
constexpr int MARGIN_MAX = CrossPointSettings::SCREEN_MARGIN_MAX;
constexpr int MARGIN_STEP = CrossPointSettings::SCREEN_MARGIN_STEP;
constexpr int WORD_SPACING_MIN = CrossPointSettings::WORD_SPACING_MIN;
constexpr int WORD_SPACING_MAX = CrossPointSettings::WORD_SPACING_MAX;
constexpr int WORD_SPACING_STEP = CrossPointSettings::WORD_SPACING_STEP;
static_assert(std::size(WORD_SPACING_IDS) == (WORD_SPACING_MAX - WORD_SPACING_MIN) / WORD_SPACING_STEP + 1);
}  // namespace

TextSettingsActivity::TextSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const SdCardFontRegistry* registry, Tab initialTab,
                                           BookTextSettings* bookSettings)
    : UiTabListActivity("TextSettings", renderer, mappedInput),
      registry_(registry),
      bookSettings_(bookSettings),
      settings_(bookSettings ? bookSettings->resolve() : ReaderTextSettings::defaults()),
      tab_(initialTab) {}

const char* TextSettingsActivity::tabLabel(const int index) const { return I18N.get(TAB_NAME_IDS[index]); }

void TextSettingsActivity::onEnter() {
  UiTabListActivity::onEnter();

  metrics_ = UITheme::getInstance().getMetrics();
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int safeBottom = safeArea.y + safeArea.height;
  afterHeader = safeArea.y + metrics_.topPadding + metrics_.headerHeight + metrics_.verticalSpacing;
  const int usableHeight = safeBottom - afterHeader - metrics_.verticalSpacing;
  previewHeight = usableHeight * metrics_.previewHeightPercent / 100;

  fonts_.clear();
  fonts_.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + (registry_ ? registry_->getFamilyCount() : 0));
  fonts_.push_back({I18N.get(StrId::STR_NOTO_SANS), true});
  if (registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      fonts_.push_back({families[i].name, false});
    }
  }

  {
    RenderLock lock;
    refreshSettings();
  }
  // Per-tab ring positions (0 = tab bar, 1..N = row). The base reset each
  // tab's nav with followOnBuild armed, so each tab's first build shows its
  // remembered selection (Family/Size open on the current item).
  for (auto& n : tabNavs) n.selected = 1;  // default to the first list row
  tabNavs[static_cast<int>(Tab::Family)].selected = currentFamilyIndex_ + 1;
  tabNavs[static_cast<int>(Tab::Size)].selected = currentSizeIndex_ + 1;
  tabNavs[static_cast<int>(tab_)].selected = 0;  // screen opens with the tab bar focused, not a list row

  rebuildRowItems();
}

// Rebuilds rowItems_ (label + actionValue) for the active tab. Structural —
// call only when tab_ or its backing data (fonts_/sizes_) changes, never from
// buildScreen(), which just refreshes rowValues_/rowItems_[].value in place.
void TextSettingsActivity::rebuildRowItems() {
  const int count = listCount();
  rowValues_.assign(count, std::string());
  rowItems_.clear();
  rowItems_.reserve(count);
  for (int i = 0; i < count; i++) {
    fui::ListItem item;
    switch (tab_) {
      case Tab::Family:
        item.label = fonts_[i].name.c_str();
        break;
      case Tab::Size:
        item.label = sizes_[i].name.c_str();
        break;
      case Tab::Layout:
        item.label = I18N.get(LAYOUT_ROW_NAME_IDS[i]);
        break;
      case Tab::Style:
        item.label = I18N.get(STYLE_ROW_NAME_IDS[i]);
        break;
      default:
        break;
    }
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
}

// Show the resident font's sizes without changing the requested size.
void TextSettingsActivity::rebuildSizeList() {
  const auto points = readerFontPointSizes(registry_, sdFontSystem.isFallback() ? "" : settings_.sdFontFamilyName);
  const uint8_t selectedPt = sdFontSystem.activePointSize();

  sizes_.clear();
  sizes_.reserve(points.size());
  currentSizeIndex_ = 0;
  for (const uint8_t pt : points) {
    // "pt" is deliberately not translated: it is the typographic unit symbol,
    // written the same way in every language CrossPoint ships.
    char label[12];
    snprintf(label, sizeof(label), "%u pt", pt);
    if (pt == selectedPt) currentSizeIndex_ = static_cast<int>(sizes_.size());
    sizes_.push_back({label, pt});
  }
}

void TextSettingsActivity::onTabAction(const int index) {
  if (optionPopup_.isActive()) return;
  if (tab_ != static_cast<Tab>(index)) {
    tab_ = static_cast<Tab>(index);
    rebuildRowItems();
    auto& n = activeNav();
    n.selected = 0;          // tab taps land with the tab bar focused (legacy tap behavior)
    n.followOnBuild = true;  // pull the new tab's viewport to its remembered selection
    requestUpdate();
  }
  // The switched-to tab repaints as the selected pill; a flash overlay on top
  // of it just repaints the pill in the focused style.
  app.clearTapFlash();
}

void TextSettingsActivity::activateIndex(const int index) {
  if (optionPopup_.isActive()) return;
  // Most rows repaint a different surface (popup, preview, new value);
  // a lingering tap flash would gray an unrelated element.
  app.clearTapFlash();
  activateRow(index);
}

bool TextSettingsActivity::handleCustomInput() {
  if (showSaveError_) {
    const auto touch = touchSnapshotFrom(mappedInput);
    if (touch.touchReleased || mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      showSaveError_ = false;
      requestUpdate();
    }
    return true;
  }
  return optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); });
}

bool TextSettingsActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ringPos() == 0) {
      switchTab();
    } else {
      activateRow(ringPos() - 1);
    }
    return true;
  }

  return false;
}

void TextSettingsActivity::buildScreen(UiScreen& screen) {
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int tabTop = afterHeader + previewHeight;
  const int rightInset = renderer.getScreenWidth() - (safeArea.x + safeArea.width);
  const int bottomInset = renderer.getScreenHeight() - (safeArea.y + safeArea.height);
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(tabTop), static_cast<int16_t>(rightInset),
                                                static_cast<int16_t>(bottomInset), static_cast<int16_t>(safeArea.x)});

  buildTabBar(screen);

  // rowItems_ (label/actionValue) was built by rebuildRowItems() when the tab
  // was last switched; only the live value text needs refreshing here, by
  // assigning into the existing rowValues_ strings (no vector growth) rather
  // than building a new items/values vector on every render.
  const int count = listCount();
  for (int i = 0; i < count; i++) {
    switch (tab_) {
      case Tab::Family:
        rowValues_[i] = (i == currentFamilyIndex_) ? tr(STR_SELECTED) : "";
        break;
      case Tab::Size:
        rowValues_[i] = (i == currentSizeIndex_) ? tr(STR_SELECTED) : "";
        break;
      case Tab::Layout:
        rowValues_[i] = layoutValueText(i);
        break;
      default:
        break;
    }
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
    if (tab_ == Tab::Style && i != static_cast<int>(StyleRow::UseDefaults)) {
      GUI.setCheckboxRow(rowItems_[i], styleRowChecked(i));
    } else if (tab_ == Tab::Layout && i == static_cast<int>(LayoutRow::ParaSpacing)) {
      GUI.setCheckboxRow(rowItems_[i], settings_.extraParagraphSpacing);
    }
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Titles match the value's font size (smallText) so both sides of a row
  // read as one unit; labels that still don't fit wrap onto a second line.
  // maxLines=2 also marks the style explicitly set (see SettingsActivity).
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncTabListViewport(screen, props);
  screen.list(props);
}

const char* TextSettingsActivity::confirmLabelText() const {
  if (ringPos() == 0) {
    // Confirm on the tab bar advances to the next tab.
    return I18N.get(TAB_NAME_IDS[(static_cast<int>(tab_) + 1) % static_cast<int>(Tab::Count)]);
  }
  switch (tab_) {
    case Tab::Layout:
      return ringPos() - 1 == static_cast<int>(LayoutRow::ParaSpacing) ? tr(STR_TOGGLE) : tr(STR_SELECT);
    case Tab::Style:
      return ringPos() - 1 == static_cast<int>(StyleRow::UseDefaults) ? tr(STR_SELECT) : tr(STR_TOGGLE);
    default:
      return tr(STR_SELECT);
  }
}

void TextSettingsActivity::drawChrome() {
  char title[96];
  snprintf(title, sizeof(title), "%s · %s", tr(STR_TEXT_SETTINGS),
           bookSettings_ ? tr(STR_THIS_BOOK) : tr(STR_DEFAULTS));
  GUI.drawHeader(renderer, UITheme::getInstance().getHeaderRect(renderer, true, false), title);

  const char* familyName = (currentFamilyIndex_ >= 0 && currentFamilyIndex_ < static_cast<int>(fonts_.size()))
                               ? fonts_[currentFamilyIndex_].name.c_str()
                               : settings_.sdFontFamilyName;
  const char* sizeName = (currentSizeIndex_ >= 0 && currentSizeIndex_ < static_cast<int>(sizes_.size()))
                             ? sizes_[currentSizeIndex_].name.c_str()
                             : "";
  char labelBuf[128];
  if (sdFontSystem.isFallback()) {
    snprintf(labelBuf, sizeof(labelBuf), "%s: %s (%s: %s, %s)", tr(STR_PREVIEW), familyName, tr(STR_FONT_FALLBACK),
             tr(STR_NOTO_SANS), sizeName);
  } else {
    snprintf(labelBuf, sizeof(labelBuf), "%s \"%s, %s\"", tr(STR_PREVIEW), familyName, sizeName);
  }
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect previewBounds{safeArea.x, afterHeader, safeArea.width, previewHeight};
  const auto [savedX, savedY, savedWidth, savedHeight] = renderer.getClipRect();
  const int clipLeft = std::max(previewBounds.x, savedX);
  const int clipTop = std::max(previewBounds.y, savedY);
  const int clipRight = std::min(previewBounds.x + previewBounds.width, savedX + savedWidth);
  const int clipBottom = std::min(previewBounds.y + previewBounds.height, savedY + savedHeight);
  renderer.setClipRect(clipLeft, clipTop, std::max(0, clipRight - clipLeft), std::max(0, clipBottom - clipTop));
  textsettings::renderPreview(renderer, settings_, fontId_, previewLayout_, metrics_.previewPadding,
                              metrics_.verticalSpacing, previewBounds, labelBuf);
  renderer.setClipRect(savedX, savedY, savedWidth, savedHeight);
}

void TextSettingsActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabelText(), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (showSaveError_) GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
}

void TextSettingsActivity::render(RenderLock&& lock) {
  if (optionPopup_.processRender(renderer, mappedInput)) return;  // picker draws over everything
  UiListActivity::render(std::move(lock));
}

// Persist before publishing the new context; rendering may still use the old font.
bool TextSettingsActivity::applySettings(const ReaderTextSettings& candidate) {
  if (!(bookSettings_ ? bookSettings_->update(candidate) : candidate.saveDefaults())) {
    LOG_ERR("TEXT", "Failed to save text settings");
    showSaveError_ = true;
    requestUpdate();
    return false;
  }
  {
    RenderLock lock;
    refreshSettings();
  }
  requestUpdate();
  return true;
}

void TextSettingsActivity::refreshSettings() {
  settings_ = bookSettings_ ? bookSettings_->resolve() : ReaderTextSettings::defaults();
  previewLayout_.lines.clear();
  previewLayout_.key = {};
  fontId_ = sdFontSystem.activate(renderer, settings_);
  currentFamilyIndex_ = findCurrentFontIndex(registry_, settings_.sdFontFamilyName);
  rebuildSizeList();
  tabNavs[static_cast<int>(Tab::Size)].selected = currentSizeIndex_ + 1;
  rebuildRowItems();
}

void TextSettingsActivity::applyFamily(int listIndex) {
  auto candidate = settings_;
  const auto& font = fonts_[listIndex];
  if (font.isBuiltin) {
    candidate.sdFontFamilyName[0] = '\0';
  } else {
    snprintf(candidate.sdFontFamilyName, sizeof(candidate.sdFontFamilyName), "%s", font.name.c_str());
  }
  applySettings(candidate);
}

void TextSettingsActivity::activateRow(int row) {
  if (showSaveError_) {
    showSaveError_ = false;
    requestUpdate();
    return;
  }
  switch (tab_) {
    case Tab::Family:
      if (row != currentFamilyIndex_) applyFamily(row);
      break;
    case Tab::Size:
      applySize(row);
      break;
    case Tab::Layout:
      confirmLayoutRow(row);
      break;
    case Tab::Style:
      confirmStyleRow(row);
      break;
    case Tab::Count:
      break;
  }
}

void TextSettingsActivity::applySize(int listIndex) {
  auto candidate = settings_;
  candidate.fontPointSize = sizes_[listIndex].pointSize;
  if (candidate.fontPointSize != settings_.fontPointSize) applySettings(candidate);
}

void TextSettingsActivity::confirmLayoutRow(int row) {
  switch (static_cast<LayoutRow>(row)) {
    case LayoutRow::ParaSpacing: {
      auto candidate = settings_;
      candidate.extraParagraphSpacing = !candidate.extraParagraphSpacing;
      applySettings(candidate);
      break;
    }
    case LayoutRow::ParaIndentation: {
      std::vector<std::string> options;
      options.reserve(6);
      options.push_back(tr(STR_STATE_OFF));
      for (int spaces = 1; spaces <= 5; ++spaces) options.push_back(std::to_string(spaces));
      optionPopup_.show(StrId::STR_PARAGRAPH_INDENTATION, options, settings_.paragraphIndentSpaces, [this](int idx) {
        auto candidate = settings_;
        candidate.paragraphIndentSpaces = static_cast<uint8_t>(idx);
        if (candidate.paragraphIndentSpaces != settings_.paragraphIndentSpaces) applySettings(candidate);
      });
      requestUpdate();
      break;
    }
    case LayoutRow::LineSpacing:
      optionPopup_.show(StrId::STR_LINE_SPACING, LINE_SPACING_IDS, static_cast<int>(std::size(LINE_SPACING_IDS)),
                        settings_.lineSpacing, [this](int idx) {
                          auto candidate = settings_;
                          candidate.lineSpacing = static_cast<uint8_t>(idx);
                          if (candidate.lineSpacing != settings_.lineSpacing) applySettings(candidate);
                        });
      requestUpdate();
      break;
    case LayoutRow::Alignment:
      optionPopup_.show(StrId::STR_ALIGNMENT, ALIGNMENT_IDS, static_cast<int>(std::size(ALIGNMENT_IDS)),
                        settings_.paragraphAlignment, [this](int idx) {
                          auto candidate = settings_;
                          candidate.paragraphAlignment = static_cast<uint8_t>(idx);
                          if (candidate.paragraphAlignment != settings_.paragraphAlignment) applySettings(candidate);
                        });
      requestUpdate();
      break;
    case LayoutRow::WordSpacing: {
      const int cur = (std::clamp<int>(settings_.wordSpacing, WORD_SPACING_MIN, WORD_SPACING_MAX) - WORD_SPACING_MIN) /
                      WORD_SPACING_STEP;
      optionPopup_.show(StrId::STR_WORD_SPACING, WORD_SPACING_IDS, static_cast<int>(std::size(WORD_SPACING_IDS)), cur,
                        [this](int idx) {
                          auto candidate = settings_;
                          candidate.wordSpacing = static_cast<uint8_t>(WORD_SPACING_MIN + idx * WORD_SPACING_STEP);
                          if (candidate.wordSpacing != settings_.wordSpacing) applySettings(candidate);
                        });
      requestUpdate();
      break;
    }
    case LayoutRow::CharacterSpacing:
      optionPopup_.show(StrId::STR_CHARACTER_SPACING, CHARACTER_SPACING_IDS,
                        static_cast<int>(std::size(CHARACTER_SPACING_IDS)), settings_.characterSpacing,
                        [this](int idx) {
                          auto candidate = settings_;
                          candidate.characterSpacing = static_cast<uint8_t>(idx);
                          if (candidate.characterSpacing != settings_.characterSpacing) applySettings(candidate);
                        });
      requestUpdate();
      break;
    case LayoutRow::ScreenMargin: {
      std::vector<std::string> options;
      options.reserve((MARGIN_MAX - MARGIN_MIN) / MARGIN_STEP + 1);
      for (int m = MARGIN_MIN; m <= MARGIN_MAX; m += MARGIN_STEP) options.push_back(std::to_string(m));
      const int cur = (std::clamp<int>(settings_.screenMargin, MARGIN_MIN, MARGIN_MAX) - MARGIN_MIN) / MARGIN_STEP;
      optionPopup_.show(StrId::STR_SCREEN_MARGIN, options, cur, [this](int idx) {
        auto candidate = settings_;
        candidate.screenMargin = static_cast<uint8_t>(MARGIN_MIN + idx * MARGIN_STEP);
        if (candidate.screenMargin != settings_.screenMargin) applySettings(candidate);
      });
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
      const uint8_t v = settings_.lineSpacing;
      return I18N.get(LINE_SPACING_IDS[v < std::size(LINE_SPACING_IDS) ? v : CrossPointSettings::LINE_SPACING_120]);
    }
    case LayoutRow::ParaIndentation: {
      const int spaces = std::clamp<int>(settings_.paragraphIndentSpaces, 0, 5);
      return spaces == 0 ? tr(STR_STATE_OFF) : std::to_string(spaces);
    }
    case LayoutRow::Alignment: {
      const uint8_t v = settings_.paragraphAlignment;
      return v < std::size(ALIGNMENT_IDS) ? I18N.get(ALIGNMENT_IDS[v]) : I18N.get(StrId::STR_JUSTIFY);
    }
    case LayoutRow::WordSpacing:
      return std::to_string(settings_.wordSpacing) + "%";
    case LayoutRow::CharacterSpacing: {
      const uint8_t v = settings_.characterSpacing;
      return v < std::size(CHARACTER_SPACING_IDS) ? I18N.get(CHARACTER_SPACING_IDS[v])
                                                  : I18N.get(StrId::STR_SPACING_ZERO);
    }
    case LayoutRow::ScreenMargin:
      return std::to_string(settings_.screenMargin);

    default:
      return "";
  }
}

void TextSettingsActivity::confirmStyleRow(int row) {
  if (static_cast<StyleRow>(row) == StyleRow::UseDefaults) {
    if (!bookSettings_ || !bookSettings_->hasOverrides()) return;
    if (!bookSettings_->reset()) {
      LOG_ERR("TEXT", "Failed to reset text settings");
      showSaveError_ = true;
    } else {
      RenderLock lock;
      refreshSettings();
    }
    requestUpdate();
    return;
  }
  auto candidate = settings_;
  switch (static_cast<StyleRow>(row)) {
    case StyleRow::FocusReading:
      candidate.focusReadingEnabled = !candidate.focusReadingEnabled;
      break;
    case StyleRow::Hyphenation:
      candidate.hyphenationEnabled = !candidate.hyphenationEnabled;
      break;
    case StyleRow::EmbeddedStyle:
      candidate.embeddedStyle = !candidate.embeddedStyle;
      break;
    case StyleRow::AntiAliasing:
      candidate.textAntiAliasing = !candidate.textAntiAliasing;
      break;
    case StyleRow::UseDefaults:
    case StyleRow::Count:
      return;
  }
  applySettings(candidate);
}

bool TextSettingsActivity::styleRowChecked(int row) const {
  switch (static_cast<StyleRow>(row)) {
    case StyleRow::FocusReading:
      return settings_.focusReadingEnabled;
    case StyleRow::Hyphenation:
      return settings_.hyphenationEnabled;
    case StyleRow::EmbeddedStyle:
      return settings_.embeddedStyle;
    case StyleRow::AntiAliasing:
      return settings_.textAntiAliasing;

    default:
      return false;
  }
}

void TextSettingsActivity::switchTab(const int direction) {
  const bool onTabBar = ringPos() == 0;
  constexpr int count = static_cast<int>(Tab::Count);
  tab_ = static_cast<Tab>((static_cast<int>(tab_) + direction + count) % count);
  rebuildRowItems();
  auto& n = activeNav();
  if (onTabBar) n.selected = 0;
  n.followOnBuild = true;  // pull the new tab's viewport to its remembered selection
  requestUpdate();
}

int TextSettingsActivity::listCount() const {
  switch (tab_) {
    case Tab::Family:
      return static_cast<int>(fonts_.size());
    case Tab::Size:
      return static_cast<int>(sizes_.size());
    case Tab::Layout:
      return static_cast<int>(LayoutRow::Count);
    case Tab::Style:
      return static_cast<int>(StyleRow::Count) - (bookSettings_ ? 0 : 1);

    default:
      return 0;
  }
}
