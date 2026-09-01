#include "SettingsActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "FontDownloadActivity.h"
#include "KOReaderSettingsActivity.h"
#include "KeyboardLayoutsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "StatusBarSettingsActivity.h"
#include "TextSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

SettingsActivity::SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Settings", renderer, mappedInput) {}

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  // Rescan /dictionaries on every rebuild: cheap (one directory listing) and
  // picks up dictionaries copied to the SD card since the last visit.
  std::vector<DictionaryEntry> dictionaries;
  DictionaryRegistry::discover(dictionaries);

  for (auto& setting : getSettingsList(&sdFontSystem.registry(), &dictionaries)) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      // The sunlight fading fix is a grayscale-waveform compensation that does
      // not apply on the X4 Pro / X4 Classic (plain OTP waveform, same panels).
      if (setting.valuePtr == &CrossPointSettings::fadingFix &&
          (BoardConfig::isX4Pro() || BoardConfig::isX4Classic())) {
        continue;
      }
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      // Settings merged into "Text Settings"
      // (they stay in the shared list for the web settings API)
      if (setting.inTextSettings) continue;
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      if (setting.valuePtr == &CrossPointSettings::pwrBtnFootnoteBack &&
          SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::FOOTNOTES) {
        continue;
      }
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      systemSettings.push_back(setting);
    }
  }

  // Append device-only ACTION items
  if (!BoardConfig::hasTouch()) {
    controlsSettings.insert(controlsSettings.begin(),
                            SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  }
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  // OTA fetches this board's own release asset (see OtaUpdater); boards whose
  // asset isn't published yet just report no update available.
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KEYBOARD_LAYOUTS, SettingAction::KeyboardLayouts));
  readerSettings.insert(readerSettings.begin(),
                        SettingInfo::Action(StrId::STR_TEXT_SETTINGS, SettingAction::TextSettings));
  readerSettings.insert(readerSettings.begin() + 1,
                        SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  // Update currentSettings pointer and count for the active category
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
  rebuildRowItems();
}

void SettingsActivity::onEnter() {
  UiTabListActivity::onEnter();

  // Reset selection to first category (ring position 0, the tab bar, comes
  // from the base's per-tab nav reset)
  selectedCategoryIndex = 0;
  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettingsLists();
}

void SettingsActivity::selectCategory(const int categoryIndex) {
  selectedCategoryIndex = categoryIndex;
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
  activeNav().top = 0;  // category switches start the list at the top (no per-tab memory here)
  rebuildRowItems();
}

// Rebuilds rowValues_/rowItems_ (label + actionValue) for *currentSettings.
// Structural — call only when the active category or a category's setting
// list changes, never from buildScreen(), which only refreshes rowValues_
// content and rowItems_[].value pointers in place.
void SettingsActivity::rebuildRowItems() {
  const auto& settings = *currentSettings;
  rowValues_.assign(settings.size(), std::string());
  rowItems_.clear();
  rowItems_.reserve(settings.size());
  for (size_t i = 0; i < settings.size(); i++) {
    fui::ListItem item;
    item.label = I18N.get(settings[i].nameId);
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
}

void SettingsActivity::onTabAction(const int index) {
  if (optionPopup.isActive()) return;
  selectCategory(index);
  activeNav().selected = 0;  // tab taps land with the tab bar focused
  // The switched-to tab repaints as the selected pill; a flash overlay on top
  // of it just repaints the pill in the focused style.
  app.clearTapFlash();
}

void SettingsActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  (void)index;  // toggleCurrentSetting reads the ring position
  // Most rows repaint a different surface (popup, sub-activity, new value);
  // a lingering tap flash would gray an unrelated element.
  app.clearTapFlash();
  toggleCurrentSetting();
  // Tap-first: a tapped row is not a cursor position. Leaving it focused
  // (inverted) after the tap meant the row stayed black once its sub-screen or
  // popup closed, and Back then had to clear that focus before a second Back
  // left Settings. Hand the focus back to the tab band; the viewport stays put.
  if (mappedInput.hasTouch()) {
    activeNav().selected = 0;
  }
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr) {
  // Theme changes take effect immediately, on this screen — reload the theme
  // and re-derive the app's tokens so the very next repaint is in the new look.
  if (valuePtr != &CrossPointSettings::uiTheme) {
    return;
  }
  UITheme::getInstance().reload();
  // Re-derive the shared tokens for the new look; the gate stays closed until
  // the repaint that rebuilds the interaction table in the new layout.
  resetUi();
}

bool SettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void SettingsActivity::stepTab(const int direction) {
  // Ring position 0 stays on the tab bar; a row selection collapses to the
  // new category's first row (per-tab memory is deliberately not kept here).
  const bool onTabBar = ringPos() == 0;
  selectedCategoryIndex = direction > 0 ? ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount)
                                        : ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
  selectCategory(selectedCategoryIndex);
  activeNav().selected = onTabBar ? 0 : 1;
  requestUpdate();
}

bool SettingsActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ringPos() == 0) {
      stepTab(1);
    } else {
      toggleCurrentSetting();
      requestUpdate();
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (ringPos() > 0) {
      activeNav().selected = 0;
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      onGoHome();
    }
    return true;
  }

  return false;
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = ringPos() - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (setting.enumValues.size() > 2) {
      const auto valuePtr = setting.valuePtr;
      optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()),
                       currentValue, [this, valuePtr, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
                         SETTINGS.*valuePtr = idx;
                         syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
                         SETTINGS.saveToFile();
                         rebuildSettingsLists();
                         applyUiSettingChange(valuePtr);
                       });
      requestUpdate();
      return;
    }
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumValues.size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    if (totalValues > 2) {
      const auto valueSetter = setting.valueSetter;
      auto onSelect = [this, valueSetter, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
        valueSetter(idx);
        syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
        SETTINGS.saveToFile();
        rebuildSettingsLists();
      };
      if (!setting.enumStringValues.empty()) {
        optionPopup.show(setting.nameId, setting.enumStringValues, cur, std::move(onSelect));
      } else {
        optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()), cur,
                         std::move(onSelect));
      }
      requestUpdate();
      return;
    }
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CheckForUpdates:
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::TextSettings:
        startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                      TextSettingsActivity::Tab::Family),
                               [this](const ActivityResult&) {
                                 // TextSettingsActivity saves on each change; no save needed here.
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::Language:
        // Row labels are translated once in rebuildRowItems() and don't
        // re-run on Pop (see ActivityManager::loop()), so a language switch
        // needs an explicit rebuild here rather than the generic resultHandler.
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::KeyboardLayouts:
        if (auto activity = makeUniqueNoThrow<KeyboardLayoutsActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(activity), nullptr);
        } else {
          LOG_ERR("SETTINGS", "OOM: KeyboardLayoutsActivity");
        }
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
  SETTINGS.saveToFile();
  rebuildSettingsLists();
  applyUiSettingChange(setting.valuePtr);
  activeNav().selected = std::min(ringPos(), settingsCount);
}

void SettingsActivity::syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged) {
  if (quickResumeTimeoutChanged) {
    preserveQuickResumeTimeoutOn =
        SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
    quickResumeTimeoutAutoEnabled = false;
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME) {
    if (SETTINGS.quickResumeSleepScreen != CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT) {
      SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      quickResumeTimeoutAutoEnabled = !preserveQuickResumeTimeoutOn;
    } else if (sleepScreenChanged && !preserveQuickResumeTimeoutOn) {
      quickResumeTimeoutAutoEnabled = true;
    }
    return;
  }

  if (sleepScreenChanged && quickResumeTimeoutAutoEnabled && !preserveQuickResumeTimeoutOn) {
    SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_NEVER;
    quickResumeTimeoutAutoEnabled = false;
  }
}

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
          CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
          StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, StrId::STR_SLEEP_NEVER),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

std::string SettingsActivity::settingValueText(const SettingInfo& setting) {
  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    return SETTINGS.*(setting.valuePtr) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  }
  if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    // Guard like the valueGetter branch below: a corrupt/migrated settings
    // byte must not index past the enum table.
    const uint8_t value = SETTINGS.*(setting.valuePtr);
    if (value >= setting.enumValues.size()) return "";
    return I18N.get(setting.enumValues[value]);
  }
  if (setting.type == SettingType::ENUM && setting.valueGetter) {
    const uint8_t value = setting.valueGetter();
    if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
      return setting.enumStringValues[value];
    }
    if (value < setting.enumValues.size()) {
      return I18N.get(setting.enumValues[value]);
    }
    return "";
  }
  if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
      if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
        return tr(STR_SLEEP_NEVER);
      }
      char valueBuffer[32];
      snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
               static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
      return valueBuffer;
    }
    return std::to_string(SETTINGS.*(setting.valuePtr));
  }
  return "";
}

void SettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});

  buildTabBar(screen);

  // rowItems_ (label/actionValue) was built by rebuildRowItems() when the
  // category was last selected/rebuilt; only the live value text needs
  // refreshing here, by assigning into the existing rowValues_ strings (no
  // vector growth) rather than building a new items/values vector on every
  // render.
  const auto& settings = *currentSettings;
  for (size_t i = 0; i < settings.size(); i++) {
    rowValues_[i] = settingValueText(settings[i]);
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Titles match the value's font size (smallText) so both sides of a row
  // read as one unit; labels that still don't fit wrap onto a second line.
  // maxLines=2 also marks the style explicitly set (an all-default smallText
  // fails textStyleUnset and the list would substitute bodyText back); the
  // common fits-on-one-line case takes the renderer's fast path anyway.
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncTabListViewport(screen, props);
  screen.list(props);
}

void SettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  // Version rides in the header's trailing label slot: the footer position
  // conflicts with button hints on non-touch devices.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE),
                 CROSSPOINT_VERSION);

  renderUi();

  const int ring = ringPos();
  const auto confirmLabel =
      (ring == 0) ? I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount])
                  : (ring > 0 && (*currentSettings)[ring - 1].nameId == StrId::STR_TIME_TO_SLEEP ? tr(STR_SELECT)
                                                                                                 : tr(STR_TOGGLE));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
