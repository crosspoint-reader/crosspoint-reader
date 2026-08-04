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
  // Bit position in the persisted mask. Assigned here rather than derived from
  // the enum value: the mask lives in settings.json and outlives firmware
  // updates, while the order of KeyboardLayoutId is the SDK's business. If a
  // layout were ever inserted mid-enum, deriving the bit would silently
  // reinterpret every saved mask -- a user who enabled Cyrillic would come back
  // to Hebrew. These numbers are a storage format: append only, never reuse or
  // renumber. 16 is the ceiling, since the mask is a uint16_t.
  uint8_t bitIndex;
  const char* code;  // ISO 639-3, shown on the language key: "ENG", "RUS", ...
  Language language;
};

inline constexpr LayoutInfo ALL[] = {
    {freeink::ui::KeyboardLayoutId::QwertyEn, 0, "ENG", Language::EN},
    {freeink::ui::KeyboardLayoutId::AzertyFr, 1, "FRA", Language::FR},
    {freeink::ui::KeyboardLayoutId::QwertzDe, 2, "DEU", Language::DE},
    {freeink::ui::KeyboardLayoutId::SpanishEs, 3, "SPA", Language::ES},
    {freeink::ui::KeyboardLayoutId::CyrillicRu, 4, "RUS", Language::RU},
    {freeink::ui::KeyboardLayoutId::CyrillicUk, 5, "UKR", Language::UK},
    {freeink::ui::KeyboardLayoutId::CyrillicBe, 6, "BEL", Language::BE},
    {freeink::ui::KeyboardLayoutId::CyrillicKk, 7, "KAZ", Language::KK},
    {freeink::ui::KeyboardLayoutId::HebrewIl, 8, "HEB", Language::HE},
};
inline constexpr uint8_t COUNT = sizeof(ALL) / sizeof(ALL[0]);

// The persisted mask is a uint16_t, so the table cannot outgrow 16 entries
// without a storage format change.
static_assert(COUNT <= 16, "keyboardLayouts mask is uint16_t; widen it before adding a 17th layout");

// Named layoutBit rather than bit: Arduino.h defines a bit(b) macro, which would
// otherwise expand this call site into a shift on an enum.
uint16_t layoutBit(freeink::ui::KeyboardLayoutId id);

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
