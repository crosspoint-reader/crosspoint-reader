#pragma once

#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "activities/settings/SettingsActivity.h"

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
inline std::vector<SettingInfo> getSettingsList() {
  return {
      // --- Display ---
      SettingInfo::Enum("Sleep Screen", &CrossPointSettings::sleepScreen,
                        {"Dark", "Light", "Custom", "Cover", "None", "Cover + Custom"}, "sleepScreen", "Display"),
      SettingInfo::Enum("Sleep Screen Cover Mode", &CrossPointSettings::sleepScreenCoverMode, {"Fit", "Crop"},
                        "sleepScreenCoverMode", "Display"),
      SettingInfo::Enum("Sleep Screen Cover Filter", &CrossPointSettings::sleepScreenCoverFilter,
                        {"None", "Contrast", "Inverted"}, "sleepScreenCoverFilter", "Display"),
      SettingInfo::Toggle("Sleep Screen Overlay", &CrossPointSettings::sleepScreenOverlay, "sleepScreenOverlay",
                          "Display"),
      SettingInfo::Enum(
          "Status Bar", &CrossPointSettings::statusBar,
          {"None", "No Progress", "Full w/ Percentage", "Full w/ Book Bar", "Book Bar Only", "Full w/ Chapter Bar"},
          "statusBar", "Display"),
      SettingInfo::Enum("Hide Battery %", &CrossPointSettings::hideBatteryPercentage, {"Never", "In Reader", "Always"},
                        "hideBatteryPercentage", "Display"),
      SettingInfo::Enum("Refresh Frequency", &CrossPointSettings::refreshFrequency,
                        {"1 page", "5 pages", "10 pages", "15 pages", "30 pages"}, "refreshFrequency", "Display"),
      SettingInfo::Enum("UI Theme", &CrossPointSettings::uiTheme, {"Classic", "Lyra"}, "uiTheme", "Display"),
      SettingInfo::Toggle("Sunlight Fading Fix", &CrossPointSettings::fadingFix, "fadingFix", "Display"),

      // --- Reader ---
      SettingInfo::Enum("Font Family", &CrossPointSettings::fontFamily, {"Bookerly", "Noto Sans", "Open Dyslexic"},
                        "fontFamily", "Reader"),
      SettingInfo::Enum("Font Size", &CrossPointSettings::fontSize, {"Small", "Medium", "Large", "X Large"}, "fontSize",
                        "Reader"),
      SettingInfo::Enum("Line Spacing", &CrossPointSettings::lineSpacing, {"Tight", "Normal", "Wide"}, "lineSpacing",
                        "Reader"),
      SettingInfo::Value("Screen Margin", &CrossPointSettings::screenMargin, {5, 40, 5}, "screenMargin", "Reader"),
      SettingInfo::Enum("Paragraph Alignment", &CrossPointSettings::paragraphAlignment,
                        {"Justify", "Left", "Center", "Right", "Book's Style"}, "paragraphAlignment", "Reader"),
      SettingInfo::Toggle("Book's Embedded Style", &CrossPointSettings::embeddedStyle, "embeddedStyle", "Reader"),
      SettingInfo::Toggle("Hyphenation", &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled", "Reader"),
      SettingInfo::Enum("Reading Orientation", &CrossPointSettings::orientation,
                        {"Portrait", "Landscape CW", "Inverted", "Landscape CCW"}, "orientation", "Reader"),
      SettingInfo::Toggle("Extra Paragraph Spacing", &CrossPointSettings::extraParagraphSpacing,
                          "extraParagraphSpacing", "Reader"),
      SettingInfo::Toggle("Text Anti-Aliasing", &CrossPointSettings::textAntiAliasing, "textAntiAliasing", "Reader"),

      // --- Controls ---
      SettingInfo::Enum("Side Button Layout (reader)", &CrossPointSettings::sideButtonLayout,
                        {"Prev, Next", "Next, Prev"}, "sideButtonLayout", "Controls"),
      SettingInfo::Toggle("Long-press Chapter Skip", &CrossPointSettings::longPressChapterSkip, "longPressChapterSkip",
                          "Controls"),
      SettingInfo::Enum("Short Power Button Click", &CrossPointSettings::shortPwrBtn, {"Ignore", "Sleep", "Page Turn"},
                        "shortPwrBtn", "Controls"),

      // --- System ---
      SettingInfo::Enum("Time to Sleep", &CrossPointSettings::sleepTimeout,
                        {"1 min", "5 min", "10 min", "15 min", "30 min"}, "sleepTimeout", "System"),

      // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
      SettingInfo::DynamicString(
          "KOReader Username", [] { return KOREADER_STORE.getUsername(); },
          [](const std::string& v) {
            KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
            KOREADER_STORE.saveToFile();
          },
          "koUsername", "KOReader Sync"),
      SettingInfo::DynamicString(
          "KOReader Password", [] { return KOREADER_STORE.getPassword(); },
          [](const std::string& v) {
            KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
            KOREADER_STORE.saveToFile();
          },
          "koPassword", "KOReader Sync"),
      SettingInfo::DynamicString(
          "Sync Server URL", [] { return KOREADER_STORE.getServerUrl(); },
          [](const std::string& v) {
            KOREADER_STORE.setServerUrl(v);
            KOREADER_STORE.saveToFile();
          },
          "koServerUrl", "KOReader Sync"),
      SettingInfo::DynamicEnum(
          "Document Matching", {"Filename", "Binary"},
          [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
          [](uint8_t v) {
            KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
            KOREADER_STORE.saveToFile();
          },
          "koMatchMethod", "KOReader Sync"),

      // --- OPDS Browser (web-only, uses CrossPointSettings char arrays) ---
      SettingInfo::String("OPDS Server URL", SETTINGS.opdsServerUrl, sizeof(SETTINGS.opdsServerUrl), "opdsServerUrl",
                          "OPDS Browser"),
      SettingInfo::String("OPDS Username", SETTINGS.opdsUsername, sizeof(SETTINGS.opdsUsername), "opdsUsername",
                          "OPDS Browser"),
      SettingInfo::String("OPDS Password", SETTINGS.opdsPassword, sizeof(SETTINGS.opdsPassword), "opdsPassword",
                          "OPDS Browser"),
  };
}
