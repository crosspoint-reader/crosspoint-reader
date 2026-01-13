#pragma once
#include <vector>

#include "CrossPointSettings.h"
#include "activities/settings/SettingsActivity.h"

// Returns the list of all settings
// This is the single source of truth used by both the device UI and web API
inline std::vector<SettingInfo> getSettingsList() {
  return {
      SettingInfo::Enum("sleepScreen", "Sleep Screen", &CrossPointSettings::sleepScreen,
                        {"Dark", "Light", "Custom", "Cover", "None"}),
      SettingInfo::Enum("sleepScreenCoverMode", "Sleep Screen Cover Mode", &CrossPointSettings::sleepScreenCoverMode,
                        {"Fit", "Crop"}),
      SettingInfo::Enum("statusBar", "Status Bar", &CrossPointSettings::statusBar, {"None", "No Progress", "Full"}),
      SettingInfo::Enum("hideBatteryPercentage", "Hide Battery %", &CrossPointSettings::hideBatteryPercentage,
                        {"Never", "In Reader", "Always"}),
      SettingInfo::Toggle("extraParagraphSpacing", "Extra Paragraph Spacing",
                          &CrossPointSettings::extraParagraphSpacing),
      SettingInfo::Toggle("textAntiAliasing", "Text Anti-Aliasing", &CrossPointSettings::textAntiAliasing),
      SettingInfo::Enum("shortPwrBtn", "Short Power Button Click", &CrossPointSettings::shortPwrBtn,
                        {"Ignore", "Sleep", "Page Turn"}),
      SettingInfo::Enum("orientation", "Reading Orientation", &CrossPointSettings::orientation,
                        {"Portrait", "Landscape CW", "Inverted", "Landscape CCW"}),
      SettingInfo::Enum("frontButtonLayout", "Front Button Layout", &CrossPointSettings::frontButtonLayout,
                        {"Bck, Cnfrm, Lft, Rght", "Lft, Rght, Bck, Cnfrm", "Lft, Bck, Cnfrm, Rght"}),
      SettingInfo::Enum("sideButtonLayout", "Side Button Layout (reader)", &CrossPointSettings::sideButtonLayout,
                        {"Prev, Next", "Next, Prev"}),
      SettingInfo::Enum("fontFamily", "Reader Font Family", &CrossPointSettings::fontFamily,
                        {"Bookerly", "Noto Sans", "Open Dyslexic"}),
      SettingInfo::Enum("fontSize", "Reader Font Size", &CrossPointSettings::fontSize,
                        {"Small", "Medium", "Large", "X Large"}),
      SettingInfo::Enum("lineSpacing", "Reader Line Spacing", &CrossPointSettings::lineSpacing,
                        {"Tight", "Normal", "Wide"}),
      SettingInfo::Value("screenMargin", "Reader Screen Margin", &CrossPointSettings::screenMargin, {5, 40, 5}),
      SettingInfo::Enum("paragraphAlignment", "Reader Paragraph Alignment", &CrossPointSettings::paragraphAlignment,
                        {"Justify", "Left", "Center", "Right"}),
      SettingInfo::Enum("sleepTimeout", "Time to Sleep", &CrossPointSettings::sleepTimeout,
                        {"1 min", "5 min", "10 min", "15 min", "30 min"}),
      SettingInfo::Enum("refreshFrequency", "Refresh Frequency", &CrossPointSettings::refreshFrequency,
                        {"1 page", "5 pages", "10 pages", "15 pages", "30 pages"}),
      SettingInfo::String("opdsServerUrl", "Calibre Web URL", SETTINGS.opdsServerUrl,
                          sizeof(SETTINGS.opdsServerUrl) - 1),
      SettingInfo::String("calibreUsername", "Calibre Username", SETTINGS.calibreUsername,
                          sizeof(SETTINGS.calibreUsername) - 1),
      SettingInfo::String("calibrePassword", "Calibre Password", SETTINGS.calibrePassword,
                          sizeof(SETTINGS.calibrePassword) - 1),
      SettingInfo::Action("Calibre Settings"),
      SettingInfo::Action("Check for updates"),
  };
}
