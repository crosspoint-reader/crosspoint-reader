#pragma once

#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "activities/settings/SettingsActivity.h"

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
//
// Uses push_back instead of initializer_list to avoid placing all SettingInfo
// temporaries (~200 bytes each) on the stack simultaneously. The initializer_list
// pattern caused a stack overflow in the 8KB loopTask on ESP32.
inline std::vector<SettingInfo> getSettingsList() {
  std::vector<SettingInfo> s;

  // --- Display ---
  s.push_back(SettingInfo::Enum("Sleep Screen", &CrossPointSettings::sleepScreen,
                                {"Dark", "Light", "Custom", "Cover", "None", "Cover + Custom"}, "sleepScreen",
                                "Display"));
  s.push_back(SettingInfo::Enum("Sleep Screen Cover Mode", &CrossPointSettings::sleepScreenCoverMode, {"Fit", "Crop"},
                                "sleepScreenCoverMode", "Display"));
  s.push_back(SettingInfo::Enum("Sleep Screen Cover Filter", &CrossPointSettings::sleepScreenCoverFilter,
                                {"None", "Contrast", "Inverted"}, "sleepScreenCoverFilter", "Display"));
  s.push_back(SettingInfo::Enum(
      "Status Bar", &CrossPointSettings::statusBar,
      {"None", "No Progress", "Full w/ Percentage", "Full w/ Book Bar", "Book Bar Only", "Full w/ Chapter Bar"},
      "statusBar", "Display"));
  s.push_back(SettingInfo::Enum("Hide Battery %", &CrossPointSettings::hideBatteryPercentage,
                                {"Never", "In Reader", "Always"}, "hideBatteryPercentage", "Display"));
  s.push_back(SettingInfo::Enum("Refresh Frequency", &CrossPointSettings::refreshFrequency,
                                {"1 page", "5 pages", "10 pages", "15 pages", "30 pages"}, "refreshFrequency",
                                "Display"));
  s.push_back(SettingInfo::Enum("UI Theme", &CrossPointSettings::uiTheme, {"Classic", "Lyra"}, "uiTheme", "Display"));
  s.push_back(SettingInfo::Toggle("Sunlight Fading Fix", &CrossPointSettings::fadingFix, "fadingFix", "Display"));

  // --- Reader ---
  s.push_back(SettingInfo::Enum("Font Family", &CrossPointSettings::fontFamily,
                                {"Bookerly", "Noto Sans", "Open Dyslexic"}, "fontFamily", "Reader"));
  s.push_back(SettingInfo::Enum("Font Size", &CrossPointSettings::fontSize, {"Small", "Medium", "Large", "X Large"},
                                "fontSize", "Reader"));
  s.push_back(SettingInfo::Enum("Line Spacing", &CrossPointSettings::lineSpacing, {"Tight", "Normal", "Wide"},
                                "lineSpacing", "Reader"));
  s.push_back(
      SettingInfo::Value("Screen Margin", &CrossPointSettings::screenMargin, {5, 40, 5}, "screenMargin", "Reader"));
  s.push_back(SettingInfo::Enum("Paragraph Alignment", &CrossPointSettings::paragraphAlignment,
                                {"Justify", "Left", "Center", "Right", "Book's Style"}, "paragraphAlignment",
                                "Reader"));
  s.push_back(
      SettingInfo::Toggle("Book's Embedded Style", &CrossPointSettings::embeddedStyle, "embeddedStyle", "Reader"));
  s.push_back(
      SettingInfo::Toggle("Hyphenation", &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled", "Reader"));
  s.push_back(SettingInfo::Enum("Reading Orientation", &CrossPointSettings::orientation,
                                {"Portrait", "Landscape CW", "Inverted", "Landscape CCW"}, "orientation", "Reader"));
  s.push_back(SettingInfo::Toggle("Extra Paragraph Spacing", &CrossPointSettings::extraParagraphSpacing,
                                  "extraParagraphSpacing", "Reader"));
  s.push_back(
      SettingInfo::Toggle("Text Anti-Aliasing", &CrossPointSettings::textAntiAliasing, "textAntiAliasing", "Reader"));

  // --- Controls ---
  s.push_back(SettingInfo::Enum("Side Button Layout (reader)", &CrossPointSettings::sideButtonLayout,
                                {"Prev, Next", "Next, Prev"}, "sideButtonLayout", "Controls"));
  s.push_back(SettingInfo::Toggle("Long-press Chapter Skip", &CrossPointSettings::longPressChapterSkip,
                                  "longPressChapterSkip", "Controls"));
  s.push_back(SettingInfo::Enum("Short Power Button Click", &CrossPointSettings::shortPwrBtn,
                                {"Ignore", "Sleep", "Page Turn"}, "shortPwrBtn", "Controls"));

  // --- System ---
  s.push_back(SettingInfo::Enum("Time to Sleep", &CrossPointSettings::sleepTimeout,
                                {"1 min", "5 min", "10 min", "15 min", "30 min"}, "sleepTimeout", "System"));

  // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
  s.push_back(SettingInfo::DynamicString(
      "KOReader Username", [] { return KOREADER_STORE.getUsername(); },
      [](const std::string& v) {
        KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
        KOREADER_STORE.saveToFile();
      },
      "koUsername", "KOReader Sync"));
  s.push_back(SettingInfo::DynamicString(
      "KOReader Password", [] { return KOREADER_STORE.getPassword(); },
      [](const std::string& v) {
        KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
        KOREADER_STORE.saveToFile();
      },
      "koPassword", "KOReader Sync"));
  s.push_back(SettingInfo::DynamicString(
      "Sync Server URL", [] { return KOREADER_STORE.getServerUrl(); },
      [](const std::string& v) {
        KOREADER_STORE.setServerUrl(v);
        KOREADER_STORE.saveToFile();
      },
      "koServerUrl", "KOReader Sync"));
  s.push_back(SettingInfo::DynamicEnum(
      "Document Matching", {"Filename", "Binary"}, [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
      [](uint8_t v) {
        KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
        KOREADER_STORE.saveToFile();
      },
      "koMatchMethod", "KOReader Sync"));

  // --- OPDS Browser (web-only, uses CrossPointSettings char arrays) ---
  s.push_back(SettingInfo::String("OPDS Server URL", SETTINGS.opdsServerUrl, sizeof(SETTINGS.opdsServerUrl),
                                  "opdsServerUrl", "OPDS Browser"));
  s.push_back(SettingInfo::String("OPDS Username", SETTINGS.opdsUsername, sizeof(SETTINGS.opdsUsername), "opdsUsername",
                                  "OPDS Browser"));
  s.push_back(SettingInfo::String("OPDS Password", SETTINGS.opdsPassword, sizeof(SETTINGS.opdsPassword), "opdsPassword",
                                  "OPDS Browser"));

  return s;
}
