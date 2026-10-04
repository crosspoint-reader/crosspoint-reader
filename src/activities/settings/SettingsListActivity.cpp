#include "SettingsListActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include "MappedInputManager.h"
#include "SettingsActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
// Row icons, indexed by SettingsSection; 32px since every row has a subtitle.
constexpr const freeink::Icon* SECTION_ICONS[SETTINGS_SECTION_COUNT] = {
    &icon_settings_32, &icon_sun_moon_32, &icon_book_open_32, &icon_pointer_32,
    &icon_folder_32,   &icon_library_32,  &icon_wifi_32,      &icon_cpu_32,
};
}  // namespace

SettingsListActivity::SettingsListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const SettingsSection openSection)
    : UiListActivity("SettingsList", renderer, mappedInput), pendingSection(openSection) {}

void SettingsListActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRows();
}

void SettingsListActivity::onExit() {
  UiListActivity::onExit();
  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsListActivity::loop() {
  if (pendingSection != SettingsSection::End) {
    const SettingsSection section = pendingSection;
    pendingSection = SettingsSection::End;
    openSection(section);
    return;
  }
  UiListActivity::loop();
}

// Rebuilds labels and name lists; call when entering and after a section
// returns (its rows or the UI language may have changed).
void SettingsListActivity::rebuildRows() {
  SettingsBySection sections;
  buildSettingsSections(sections);
  for (size_t i = 0; i < SETTINGS_SECTION_COUNT; ++i) {
    auto& names = nameLists_[i];
    names.clear();
    for (const auto& setting : sections[i]) {
      if (!names.empty()) names += ", ";
      names += I18N.get(setting.nameId);
    }
    auto& item = rowItems_[i];
    item.label = I18N.get(SETTINGS_SECTION_TITLES[i]);
    item.subtitle = nullptr;
    item.icon = fui::bitmapFromIcon(*SECTION_ICONS[i]);
    item.actionValue = static_cast<int16_t>(i);
  }
  subtitleWidth_ = -1;
}

// Picks each row's subtitle: the setting names when they wrap into at most
// two lines at the list's text width, otherwise the section summary.
void SettingsListActivity::chooseSubtitles(UiScreen& screen, const fui::ListProps& props) {
  const auto& theme = screen.theme();
  // Subtitles span the row's content width; reserve the scroll indicator too
  // so the choice doesn't flip when the list starts to scroll.
  const int16_t width =
      static_cast<int16_t>(screen.contentRect().width - 2 * (theme.listInset + theme.listSidePadding) -
                           theme.listScrollWidth - theme.listScrollInset);
  if (width == subtitleWidth_) return;
  subtitleWidth_ = width;

  const fui::TextStyle& style = props.subtitleText;
  fui::TextStyle probe = style;
  probe.maxLines = static_cast<uint8_t>(style.maxLines + 1);  // one extra line reveals overflow
  const int16_t maxHeight = static_cast<int16_t>(style.maxLines * screen.target().lineHeight(style.font));
  for (size_t i = 0; i < SETTINGS_SECTION_COUNT; ++i) {
    const auto& names = nameLists_[i];
    // The icon and its gap sit beside the subtitle, narrowing it.
    const auto& icon = rowItems_[i].icon;
    const int16_t textWidth = static_cast<int16_t>(width - (icon ? icon.width + props.textGap : 0));
    const bool fits = !names.empty() && textWidth > 0 &&
                      fui::measureWrappedText(screen.target(), names.c_str(), probe, textWidth).height <= maxHeight;
    rowItems_[i].subtitle = fits ? names.c_str() : I18N.get(SETTINGS_SECTION_SUMMARIES[i]);
  }
}

void SettingsListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  chooseSubtitles(screen, props);
  syncListViewport(screen, props);
  screen.list(props);
}

void SettingsListActivity::activateIndex(const int index) {
  // The section screen replaces this one; a lingering flash would gray an
  // unrelated element on return.
  app.clearTapFlash();
  openSection(static_cast<SettingsSection>(index));
}

void SettingsListActivity::openSection(const SettingsSection section) {
  nav.selected = static_cast<int>(section);
  auto activity = makeUniqueNoThrow<SettingsActivity>(renderer, mappedInput, section);
  if (!activity) {
    LOG_ERR("SETTINGS", "OOM: SettingsActivity");
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) {
    rebuildRows();
    // A theme change inside the section must restyle this screen too.
    resetUi();
    requestUpdate();
  });
}

void SettingsListActivity::onBackButton() { onGoHome(); }

void SettingsListActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Version rides in the header's trailing label slot: the footer position
  // conflicts with button hints on non-touch devices.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_SETTINGS_TITLE), CROSSPOINT_VERSION);
}
