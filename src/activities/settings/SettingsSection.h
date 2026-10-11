#pragma once

#include <I18n.h>

#include <cstddef>
#include <cstdint>

// Top-level groups on the Settings screen, in display order. End doubles as
// "no section" where one is optional.
enum class SettingsSection : uint8_t { General, Display, Reader, Controls, FileBrowser, Library, Network, System, End };

inline constexpr size_t SETTINGS_SECTION_COUNT = static_cast<size_t>(SettingsSection::End);

inline constexpr StrId SETTINGS_SECTION_TITLES[SETTINGS_SECTION_COUNT] = {
    StrId::STR_SETTINGS_GENERAL,      StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,       StrId::STR_CAT_CONTROLS,
    StrId::STR_SETTINGS_FILE_BROWSER, StrId::STR_LIBRARY,     StrId::STR_SETTINGS_NETWORK, StrId::STR_SETTINGS_SYSTEM,
};
