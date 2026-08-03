#pragma once

#include <FreeInkUI.h>
#include <I18n.h>

#include <cstdint>

// Which keyboard layouts a user can reach, and in what order the language key
// cycles through them.
//
// The set lives in CrossPointSettings::keyboardLayouts as one bit per
// freeink::ui::KeyboardLayoutId. Zero means unconfigured, in which case the set is
// derived from the UI language: that language's layout plus English. A user who
// never opens the setting therefore gets a plain two-way toggle, and one who
// reads in a third language can add its layout without the firmware guessing.
namespace keyboard_layouts {

// Every layout the SDK ships, in the order the language key cycles them and the
// settings screen lists them. Extending the SDK enum means extending this table.
// The name shown in settings comes from the matching UI language, so adding a
// layout needs no new i18n keys.
struct LayoutInfo {
  freeink::ui::KeyboardLayoutId id;
  const char* code;  // ISO 639-3, shown on the language key: "ENG", "RUS", ...
  Language language;
};

inline constexpr LayoutInfo ALL[] = {
    {freeink::ui::KeyboardLayoutId::QwertyEn, "ENG", Language::EN},
    {freeink::ui::KeyboardLayoutId::AzertyFr, "FRA", Language::FR},
    {freeink::ui::KeyboardLayoutId::QwertzDe, "DEU", Language::DE},
    {freeink::ui::KeyboardLayoutId::SpanishEs, "SPA", Language::ES},
    {freeink::ui::KeyboardLayoutId::CyrillicRu, "RUS", Language::RU},
    {freeink::ui::KeyboardLayoutId::CyrillicUk, "UKR", Language::UK},
    {freeink::ui::KeyboardLayoutId::CyrillicBe, "BEL", Language::BE},
    {freeink::ui::KeyboardLayoutId::CyrillicKk, "KAZ", Language::KK},
    {freeink::ui::KeyboardLayoutId::HebrewIl, "HEB", Language::HE},
};
inline constexpr uint8_t COUNT = sizeof(ALL) / sizeof(ALL[0]);

inline constexpr uint16_t bit(const freeink::ui::KeyboardLayoutId id) {
  return static_cast<uint16_t>(1u << static_cast<uint8_t>(id));
}

// The layout matching a UI language, or QwertyEn when that language has none.
// Does not consider whether that layout is enabled -- see startingLayout.
freeink::ui::KeyboardLayoutId forLanguage(Language language);

// The layout a keyboard should open on: the UI language's, or the first enabled
// one when the user has switched that off. Never returns a disabled layout.
freeink::ui::KeyboardLayoutId startingLayout(Language language);

// The configured set, or the derived default when nothing is configured.
// Always contains at least one layout.
uint16_t enabled();

// How many layouts are enabled. One means the language key has nowhere to go
// and should not be drawn at all.
uint8_t enabledCount();

// The next enabled layout after `current`, wrapping around. Returns `current`
// unchanged when it is the only one enabled.
freeink::ui::KeyboardLayoutId next(freeink::ui::KeyboardLayoutId current);

// Two-letter code for the language key's label.
const char* codeFor(freeink::ui::KeyboardLayoutId id);

}  // namespace keyboard_layouts
