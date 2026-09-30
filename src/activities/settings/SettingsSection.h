#pragma once

#include <I18n.h>

#include <cstddef>
#include <cstdint>

// Top-level groups on the Settings screen, in display order. Count doubles as
// "no section" where one is optional.
enum class SettingsSection : uint8_t { General, Display, Reader, Controls, Library, Network, Software, Count };

inline constexpr size_t SETTINGS_SECTION_COUNT = static_cast<size_t>(SettingsSection::Count);

inline constexpr StrId SETTINGS_SECTION_TITLES[SETTINGS_SECTION_COUNT] = {
    StrId::STR_SETTINGS_GENERAL,  StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,        StrId::STR_CAT_CONTROLS,
    StrId::STR_LIBRARY,           StrId::STR_SETTINGS_NETWORK, StrId::STR_SETTINGS_SOFTWARE,
};

// Subtitle used when the section's comma-joined setting names don't fit.
inline constexpr StrId SETTINGS_SECTION_SUMMARIES[SETTINGS_SECTION_COUNT] = {
    StrId::STR_SETTINGS_GENERAL_DESC, StrId::STR_SETTINGS_DISPLAY_DESC, StrId::STR_SETTINGS_READER_DESC,
    StrId::STR_SETTINGS_CONTROLS_DESC, StrId::STR_SETTINGS_LIBRARY_DESC, StrId::STR_SETTINGS_NETWORK_DESC,
    StrId::STR_SETTINGS_SOFTWARE_DESC,
};
