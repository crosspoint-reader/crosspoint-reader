#pragma once

#include <FreeInkUI.h>
#include <I18n.h>

#include <cstdint>

// Which keyboard layouts the language key cycles through.
//
// The set is a bit mask in CrossPointSettings::keyboardLayouts. Zero means
// unconfigured and resolves to the UI language's layout plus English, so a user
// who never opens the setting gets a plain two-way toggle.
namespace keyboard_layouts {

// Every layout the SDK ships, in the order the language key cycles them and the
// settings screen lists them. Rows are named after their language, so adding a
// layout needs no new i18n key.
struct LayoutInfo {
  freeink::ui::KeyboardLayoutId id;
  // Position in the persisted mask -- a storage format: append only, never
  // reuse or renumber. Not derived from the enum, whose order is the SDK's
  // business: a layout inserted mid-enum would reinterpret every saved mask.
  uint8_t bitIndex;
  Language language;
  // Latin layouts can type a Wi-Fi passphrase or a URL; the others cannot, as
  // the symbol layers hold only digits and punctuation. One always stays on.
  bool latin;
};

inline constexpr LayoutInfo ALL[] = {
    {freeink::ui::KeyboardLayoutId::QwertyEn, 0, Language::EN, true},
    {freeink::ui::KeyboardLayoutId::AzertyFr, 1, Language::FR, true},
    {freeink::ui::KeyboardLayoutId::QwertzDe, 2, Language::DE, true},
    {freeink::ui::KeyboardLayoutId::SpanishEs, 3, Language::ES, true},
    {freeink::ui::KeyboardLayoutId::CyrillicRu, 4, Language::RU, false},
    {freeink::ui::KeyboardLayoutId::CyrillicUk, 5, Language::UK, false},
    {freeink::ui::KeyboardLayoutId::CyrillicBe, 6, Language::BE, false},
    {freeink::ui::KeyboardLayoutId::CyrillicKk, 7, Language::KK, false},
    {freeink::ui::KeyboardLayoutId::HebrewIl, 8, Language::HE, false},
};
inline constexpr uint8_t COUNT = sizeof(ALL) / sizeof(ALL[0]);

// Mask bit of a table row. Named bitAt rather than bit: Arduino.h defines a
// bit(b) macro that would swallow the call.
inline constexpr uint16_t bitAt(const uint8_t i) { return static_cast<uint16_t>(1u << ALL[i].bitIndex); }

// Mask bit of a layout, or 0 for an id the table does not list -- keeping it
// out of every test rather than aliasing onto another layout's bit.
uint16_t layoutBit(freeink::ui::KeyboardLayoutId id);

// The configured set, or the derived default when nothing is configured. Never
// empty, and always holds a Latin layout.
uint16_t enabled();

// Whether `mask` holds a Latin layout.
bool hasLatin(uint16_t mask);

// How many layouts are enabled. One means the language key has nowhere to go.
uint8_t enabledCount();

// The layout a keyboard opens on: the UI language's, or an enabled one when the
// user has switched that off.
freeink::ui::KeyboardLayoutId startingLayout(Language language);

// The next enabled layout after `current`, wrapping around. Returns `current`
// when it is the only one enabled.
freeink::ui::KeyboardLayoutId next(freeink::ui::KeyboardLayoutId current);

}  // namespace keyboard_layouts
