#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <Logging.h>

#include <cstring>

#include "BuildFeatures.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "SettingActionDispatch.h"
#include "SettingsList.h"
#include "SettingsSubmenuActivity.h"
#include "TouchUi.h"
#include "UiFontScale.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

bool SettingsActivity::isRowSelectable(const int index) const {
  return index >= 0 && index < settingsCount && !(*currentSettings)[index].isSeparator;
}

bool SettingsActivity::releaseBuriedState() {
  // The four tabs' rows, the submenus and the visible window's labels: ~80 SettingInfo rows with
  // their label vectors, in many small blocks across the heap. onEnter() builds them all again.
  std::vector<SettingInfo>().swap(displaySettings);
  std::vector<SettingInfo>().swap(readerSettings);
  std::vector<SettingInfo>().swap(controlsSettings);
  std::vector<SettingInfo>().swap(systemSettings);
  std::vector<SettingInfo::SubmenuData>().swap(submenuData);
  for (std::string& label : windowLabels) std::string().swap(label);
  for (std::string& value : windowValues) std::string().swap(value);
  currentSettings = nullptr;
  settingsCount = 0;
  windowCount = 0;
  return true;
}

void SettingsActivity::onEnter() {
  needsHalfRefresh = true;

  // Build per-category vectors from the shared settings list.
  // addTo tracks the last subcategory per vector and automatically inserts a separator
  // row whenever a setting carries a new subcategory label.
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();
  submenuData.clear();
  displaySettings.reserve(20);
  readerSettings.reserve(30);
  controlsSettings.reserve(8);
  systemSettings.reserve(20);
  submenuData.reserve(4);

  // Rows go in plain; the subcategory separators are inserted at the END, once prepareSubmenus
  // has moved whatever it is going to move into submenus.
  //
  // Inserting them here instead put headings in front of rows that were about to leave. The
  // gesture rows are the case that showed it: four groups' worth of headings went into the
  // Controls tab, then all twenty rows moved into the Gesture actions submenu, and three of the
  // four headings stayed behind with nothing underneath them. Separators describe a list, so
  // they can only be computed once the list has stopped changing -- which is the order
  // MenuListActivity has always used.
  auto addTo = [](std::vector<SettingInfo>& vec, const SettingInfo& s) { vec.push_back(s); };
  auto addToMoved = [](std::vector<SettingInfo>& vec, SettingInfo s) { vec.push_back(std::move(s)); };

  bool sawReaderFontSection = false;
  bool insertedFontDownload = false;
  bool sawIncludeBetaUpdates = false;
  SettingInfo includeBetaUpdatesSetting{};

  auto insertFontDownloadBelowFontSection = [&]() {
    auto fontDownload = SettingInfo::Action(StrId::STR_FONT_MANAGER, SettingAction::DownloadFonts);
    fontDownload.withSubcategory(StrId::STR_MENU_READER_FONT);
    addToMoved(readerSettings, std::move(fontDownload));
    insertedFontDownload = true;
  };

  forEachSetting([&](const SettingInfo& setting) {
    if (setting.category == StrId::STR_NONE_OPT) return;
    if (setting.category == StrId::STR_CAT_SYSTEM &&
        (setting.nameId == StrId::STR_USE_CLOCK || setting.nameId == StrId::STR_CLOCK_FORMAT ||
         setting.nameId == StrId::STR_TIMEZONE)) {
      return;
    }
    // Enrich font-family entries with SD card families discovered at boot.
    // The row is the walk's temporary; the screen keeps its own copy, and only that is enriched.
    SettingInfo enriched = setting;
    if (setting.key &&
        (std::strcmp(setting.key, "fontFamily") == 0 || std::strcmp(setting.key, "txtFontFamily") == 0)) {
      const uint8_t n = fontFamilyOptionCount();
      enriched.enumLabels.clear();
      enriched.enumLabels.reserve(n);
      for (uint8_t i = 0; i < n; i++) enriched.enumLabels.push_back(fontFamilyOptionLabel(i));
    }
    if (enriched.nameId == StrId::STR_INCLUDE_BETA_UPDATES) {
      includeBetaUpdatesSetting = enriched;
      sawIncludeBetaUpdates = true;
      return;
    }
    const bool isReaderFontEntry =
        enriched.category == StrId::STR_CAT_READER &&
        (enriched.submenu == StrId::STR_MENU_READER_FONT || enriched.submenu == StrId::STR_MENU_TXT_FONT);

    if (!insertedFontDownload && sawReaderFontSection && !isReaderFontEntry) {
      insertFontDownloadBelowFontSection();
    }

    if (enriched.category == StrId::STR_CAT_DISPLAY) {
      addTo(displaySettings, enriched);
    } else if (enriched.category == StrId::STR_CAT_READER) {
      addTo(readerSettings, enriched);
    } else if (enriched.category == StrId::STR_CAT_CONTROLS) {
      addTo(controlsSettings, enriched);
    } else if (enriched.category == StrId::STR_CAT_SYSTEM) {
      addTo(systemSettings, enriched);
    }
    if (isReaderFontEntry) sawReaderFontSection = true;

    // Web-only categories (KOReader Sync, OPDS Browser) are skipped for device UI
  });

  if (!insertedFontDownload && sawReaderFontSection) {
    insertFontDownloadBelowFontSection();
  }

  // Device-only ACTION items — subcategory drives separator insertion automatically.
  controlsSettings.insert(controlsSettings.begin(),
                          SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  controlsSettings.insert(controlsSettings.begin(), SettingInfo::Separator(StrId::STR_MENU_BTN_PHYSICAL));

  // Button Actions overview lives at the end of the Button Actions section (same subcategory as
  // the per-button submenus, so no new separator is inserted).
  addToMoved(controlsSettings,
             std::move(SettingInfo::Action(StrId::STR_BTN_ACTIONS_OVERVIEW, SettingAction::ButtonActionsOverview)
                           .withSubcategory(StrId::STR_MENU_BTN_ACTIONS)));

#if CP_TOUCH_UI
  // The same thing for touch: where the zones are and what each currently does. Sits beside
  // the gesture rows it summarises. Inside the CP_TOUCH_UI bracket with every other touch-UI
  // row, so a non-touch build does not construct a SettingInfo it will only delete again --
  // the runtime requiring() below is for a multi-board binary, not a substitute for the gate.
  addToMoved(controlsSettings,
             std::move(SettingInfo::Action(StrId::STR_GEST_ACTIONS_OVERVIEW, SettingAction::GestureActionsOverview)
                           .withSubcategory(StrId::STR_MENU_GESTURE_ACTIONS)
                           .requiring(SettingRequires::TouchPanel)));
#endif  // CP_TOUCH_UI

  addToMoved(readerSettings, SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  addToMoved(systemSettings, SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  addToMoved(systemSettings, SettingInfo::Action(StrId::STR_KEYBOARD_LAYOUTS, SettingAction::KeyboardLayouts));
  addToMoved(systemSettings, std::move(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network)
                                           .withSubcategory(StrId::STR_MENU_SYS_NETWORK)));
#if CP_FEATURE_KOSYNC
  addToMoved(systemSettings, std::move(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync)
                                           .withSubcategory(StrId::STR_MENU_SYS_NETWORK)));
#endif
  addToMoved(systemSettings, std::move(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser)
                                           .withSubcategory(StrId::STR_MENU_SYS_NETWORK)));
  addToMoved(systemSettings, std::move(SettingInfo::Action(StrId::STR_CLOCK_SETTINGS, SettingAction::ClockSettings)
                                           .withSubcategory(StrId::STR_MENU_SYS_TOOLS)));
#if CP_FEATURE_WEATHER
  addToMoved(systemSettings, std::move(SettingInfo::Action(StrId::STR_WEATHER_SETTINGS, SettingAction::Weather)
                                           .withSubcategory(StrId::STR_MENU_SYS_TOOLS)));
#endif
  // Every file on the card, images and sidecars included. Housekeeping rather than reading, so it
  // lives here and not on Home, whose browser lists only books.
  addToMoved(systemSettings, std::move(SettingInfo::Action(StrId::STR_ALL_FILES, SettingAction::AllFiles)
                                           .withSubcategory(StrId::STR_MENU_SYS_TOOLS)));
  addToMoved(systemSettings, std::move(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache)
                                           .withSubcategory(StrId::STR_MENU_SYS_SYSTEM)));
  addToMoved(systemSettings, std::move(SettingInfo::Action(StrId::STR_SCREEN_REPAIR, SettingAction::ScreenRepair)
                                           .withSubcategory(StrId::STR_MENU_SYS_SYSTEM)));

  addToMoved(systemSettings,
             std::move(SettingInfo::Separator(StrId::STR_SYSTEM_UPDATE_TYPE1).withSubmenu(StrId::STR_SYSTEM_UPDATE)));
#if CP_FEATURE_OTA
  addToMoved(systemSettings, std::move(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates)
                                           .withSubmenu(StrId::STR_SYSTEM_UPDATE)));
  if (sawIncludeBetaUpdates) {
    addToMoved(systemSettings, std::move(includeBetaUpdatesSetting.withSubmenu(StrId::STR_SYSTEM_UPDATE)));
  }
#endif
  addToMoved(systemSettings,
             std::move(SettingInfo::Separator(StrId::STR_SYSTEM_UPDATE_TYPE2).withSubmenu(StrId::STR_SYSTEM_UPDATE)));
  addToMoved(systemSettings, std::move(SettingInfo::Action(StrId::STR_READING_STATS, SettingAction::ReadingStats)
                                           .withSubcategory(StrId::STR_MENU_SYS_SYSTEM)));
  addToMoved(systemSettings,
             std::move(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate)
                           .withSubmenu(StrId::STR_SYSTEM_UPDATE)));
  // addToMoved(systemSettings,
  //            std::move(SettingInfo::Action(StrId::STR_SWITCH_TO_USB_DRIVE, SettingAction::SwitchToUsbDrive)
  //                          .withSubmenu(StrId::STR_SYSTEM_UPDATE)));
  addToMoved(systemSettings, std::move(SettingInfo::Action(StrId::STR_SYSTEM_INFO, SettingAction::SystemInfo)
                                           .withSubcategory(StrId::STR_MENU_SYS_SYSTEM)));
  // Next to System Information, which is where anyone chasing a "the power button did
  // nothing" report already looks.
  addToMoved(systemSettings, std::move(SettingInfo::Action(StrId::STR_BOOT_DIAGNOSTICS, SettingAction::BootDiagnostics)
                                           .withSubcategory(StrId::STR_MENU_SYS_SYSTEM)));
  // A comparison screen, not a setting: it decides whether the size ladder can ship fewer real
  // faces and scale the rest, which the coverage metrics in bench/font_main.cpp cannot settle.
#if CP_FEATURE_DEV_SCREENS
  addToMoved(systemSettings, std::move(SettingInfo::Action(StrId::STR_FONT_SCALING_TEST, SettingAction::FontScalingTest)
                                           .withSubcategory(StrId::STR_MENU_SYS_SYSTEM)));
#endif

  SettingInfo::prepareSubmenus(displaySettings, submenuData);
  SettingInfo::prepareSubmenus(readerSettings, submenuData);
  SettingInfo::prepareSubmenus(controlsSettings, submenuData);
  SettingInfo::prepareSubmenus(systemSettings, submenuData);

  // Now that the tabs hold exactly the rows they will show, group them. prepareSubmenus has done
  // the same for each submenu's own rows.
  SettingInfo::insertSubcategorySeparators(displaySettings);
  SettingInfo::insertSubcategorySeparators(readerSettings);
  SettingInfo::insertSubcategorySeparators(controlsSettings);
  SettingInfo::insertSubcategorySeparators(systemSettings);

  resetUi();
  // Opens on the Display tab with the bar focused; TabbedUiListActivity::onEnter() wires the row
  // and tab actions, points the screen at buildScreen(), and puts focus on the bar.
  enterCategory(0);
  TabbedUiListActivity::onEnter();
  // Another tab when asked for one (the serial CMD:ACT:SETTINGS:<tab>), the way a tap on it goes.
  if (initialTab > 0) selectTab(initialTab);
}

void SettingsActivity::onExit() {
  TabbedUiListActivity::onExit();
  // Both read SETTINGS, so they are independent — but they are a pair: the ladder binds the UI
  // fonts and reload() re-derives the metrics sized to hold them. Changing one without the other
  // leaves rows that clip their own text.
  applyUiFontScale();               // Re-bind the UI font ladder in case the size was changed
  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::enterCategory(const int categoryIndex) {
  switch (categoryIndex) {
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
  listTapActivation.reset();
}

void SettingsActivity::activateIndex(const int index) {
  if (index < 0 || index >= settingsCount) {
    return;
  }
  app.clearTapFlash();

  const auto& setting = (*currentSettings)[index];
  if (setting.isSeparator) return;

  if (setting.usesSelectorActivity) {
    auto selector = createSelectorActivity(setting, renderer, mappedInput);
    if (selector) {
      startActivityForResult(std::move(selector), [this](const ActivityResult&) {
        CrossPointSettings::normalizeDependentSettings(SETTINGS);
        SETTINGS.saveToFile();
        needsHalfRefresh = true;
      });
    }
    return;
  }

  if (setting.type == SettingType::ACTION && tryOpenSliderFor(setting.action, [this] { needsHalfRefresh = true; })) {
    return;
  }

  if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult& result, const bool usedWifi) {
      CrossPointSettings::normalizeDependentSettings(SETTINGS);
      SETTINGS.saveToFile();
      if (usedWifi) restartToSettingsIfRadioLeftOn();
      needsHalfRefresh = true;
      const auto* menuResult = std::get_if<MenuResult>(&result.data);
      if (menuResult && menuResult->action != -1) {
        auto activity = createActivityForAction(static_cast<SettingAction>(menuResult->action), renderer, mappedInput);
        if (activity) {
          const bool nextUsedWifi = activity->usesWifi();
          startActivityForResult(std::move(activity), [this, nextUsedWifi](const ActivityResult&) {
            CrossPointSettings::normalizeDependentSettings(SETTINGS);
            SETTINGS.saveToFile();
            if (nextUsedWifi) restartToSettingsIfRadioLeftOn();
            needsHalfRefresh = true;
          });
        }
      }
    };

    if (setting.action == SettingAction::Submenu) {
      auto it = std::find_if(submenuData.begin(), submenuData.end(),
                             [&setting](const SettingInfo::SubmenuData& d) { return d.id == setting.nameId; });
      if (it != submenuData.end()) {
        startActivityForResult(
            std::make_unique<SettingsSubmenuActivity>(renderer, mappedInput, setting.nameId, it->items),
            [this](const ActivityResult&) { needsHalfRefresh = true; });
      }
    } else {
      auto activity = createActivityForAction(setting.action, renderer, mappedInput);
      if (activity) {
        const bool usedWifi = activity->usesWifi();
        startActivityForResult(std::move(activity), [resultHandler, usedWifi](const ActivityResult& result) {
          resultHandler(result, usedWifi);
        });
      }
    }
    return;
  }

  setting.toggleValue();
  CrossPointSettings::normalizeDependentSettings(SETTINGS);
  SETTINGS.saveToFile();
  // Repaint: nothing else will. Every other way this list changes asks for an
  // update -- moveSelectionTo(), the swipe handler, routeListTouch() -- but the
  // Confirm path in the list controller calls activateIndex() and
  // returns, so an inline toggle would change and persist the value while the
  // row kept showing the old one until some later event forced a render.
  // MenuListActivity::toggleCurrentItem() does this too, which is why the
  // submenus were never affected.
  //
  // Whole-screen rather than the one row on purpose: normalizeDependentSettings()
  // may have changed other rows' values as well.
  requestUpdate();
}

void SettingsActivity::materializeListWindow() {
  windowFirst = static_cast<uint16_t>(std::max(0, std::min(activeNav().top, settingsCount)));
  windowCount = static_cast<uint16_t>(
      std::min(static_cast<size_t>(settingsCount - windowFirst), static_cast<size_t>(LIST_WINDOW_CAPACITY)));
  for (uint16_t offset = 0; offset < windowCount; ++offset) {
    const size_t index = windowFirst + offset;
    const auto& setting = (*currentSettings)[index];
    windowLabels[offset] =
        setting.isSeparator && setting.nameId != StrId::STR_NONE_OPT ? I18N.get(setting.nameId) : setting.getTitle();
    // A TOGGLE row gets fui::list's switch rather than the words ON/OFF: ListItem::toggle
    // replaces the value slot with the same widget ToggleRowProps draws, and the switch visuals
    // are already inside list(), so this is a flag rather than new drawing code. The value string
    // is skipped for those rows -- list() ignores it when toggle is set, so building it would be
    // a std::string per row per render for nothing.
    const bool isToggle = !setting.isSeparator && setting.type == SettingType::TOGGLE;
    windowValues[offset] = isToggle ? std::string{} : setting.getDisplayValue();
    auto& row = windowItems[offset];
    row = {};
    row.label = windowLabels[offset].c_str();
    row.value = windowValues[offset].empty() ? nullptr : windowValues[offset].c_str();
    row.toggle = isToggle;
    row.toggleChecked = isToggle && setting.getToggleState();
    row.actionValue = static_cast<int16_t>(index);
    row.enabled = !setting.isSeparator;
    row.isHeader = setting.isSeparator;
  }
}

void SettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = listContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(contentRect.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (contentRect.x + contentRect.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (contentRect.y + contentRect.height)),
                  static_cast<int16_t>(contentRect.x)});

  buildTabBar(screen);
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));

  auto props = listProps(screen);
  props.count = static_cast<uint16_t>(settingsCount);
  syncListViewport(screen, props);
  materializeListWindow();
  props.items = windowItems.data();
  props.itemsWindowFirst = windowFirst;
  props.itemsWindowCount = windowCount;
  noteListSelection(props);
  screen.list(props);
}

int16_t SettingsActivity::tabBarHeight() const {
  return static_cast<int16_t>(UITheme::getInstance().getMetrics().tabBarHeight);
}

void SettingsActivity::onBackFromTabs() {
  SETTINGS.saveToFile();
  onGoHome();
}

void SettingsActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = listContentRect();
  GUI.drawHeader(renderer,
                 Rect{contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_SETTINGS_TITLE), CROSSPOINT_VERSION);
}

const char* SettingsActivity::footerConfirmLabel() const {
  // Confirm means "next tab" while the bar holds focus and "toggle" on a row, so the hint names
  // the category it would move to rather than a generic label.
  return selectedPosition() == 0 ? I18N.get(categoryNames[(selectedTab() + 1) % categoryCount]) : tr(STR_TOGGLE);
}

// Identical to UiListActivity::render() apart from the last line: this screen returns from a
// settings change that may have altered the theme, and on X3 the first frame after that needs a
// HALF refresh to clear the previous look rather than the FAST one the base ships.
void SettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  drawChrome();
  renderUi();
  for (int pass = 0; activeNav().consumeRebuildNeeded() && pass < 8; ++pass) {
    renderer.clearScreen();
    drawChrome();
    renderUi();
  }
  publishListWindow();
  drawFooter();

  // Only spend the HALF if enough FAST refreshes have piled up since the last one to have
  // reintroduced ghosting. needsHalfRefresh is armed on entry AND on returning from every
  // child activity, so without this gate bouncing in and out of a submenu paid a 2186 ms HALF
  // per return against a FAST's 435 ms (X3-measured), even when the previous HALF was three
  // updates ago. Correctness HALFs are armed via setNextDisplayRefreshMode() and bypass this
  // entirely -- see GfxRenderer::ghostClearingHalfWorthwhile().
  const bool halfRefresh =
      renderer.ghostsUnderFastRefresh() && needsHalfRefresh && renderer.ghostClearingHalfWorthwhile();
  // Cleared whether or not it was spent, i.e. a gated request is DROPPED, not deferred. Deferring
  // would land the 2.2 s stall a few cursor moves into the screen, which reads as a freeze; on
  // entry it reads as the screen change it is. The ghosting cost of dropping is marginal because
  // the gate only fires when a HALF ran within the last few updates.
  needsHalfRefresh = false;
  renderer.displayBuffer(halfRefresh ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
