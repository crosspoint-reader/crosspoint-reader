#include "KeyboardLayoutSet.h"

#include <bit>

#include "CrossPointSettings.h"

namespace keyboard_layouts {

namespace {

// Index into ALL for a layout id, or COUNT when the id is not listed.
uint8_t indexOf(const freeink::ui::KeyboardLayoutId id) {
  for (uint8_t i = 0; i < COUNT; ++i) {
    if (ALL[i].id == id) return i;
  }
  return COUNT;
}

// The layout matching a UI language, or QwertyEn when it has none. Says nothing
// about whether that layout is enabled -- see startingLayout().
freeink::ui::KeyboardLayoutId forLanguage(const Language language) {
  for (uint8_t i = 0; i < COUNT; ++i) {
    if (ALL[i].language == language) return ALL[i].id;
  }
  return freeink::ui::KeyboardLayoutId::QwertyEn;
}

// Two layouts sharing a bit would silently toggle each other; a gap is harmless
// (it just means a retired layout). Uniqueness plus the 16-bit range also caps
// the table at 16 rows, which is all the mask can hold.
constexpr bool bitIndicesValid() {
  for (uint8_t i = 0; i < COUNT; ++i) {
    if (ALL[i].bitIndex >= 16) return false;
    for (uint8_t j = static_cast<uint8_t>(i + 1); j < COUNT; ++j) {
      if (ALL[i].bitIndex == ALL[j].bitIndex) return false;
    }
  }
  return true;
}
static_assert(bitIndicesValid(), "layout bits must be unique and fit the uint16_t mask");

constexpr uint16_t allBits() {
  uint16_t m = 0;
  for (uint8_t i = 0; i < COUNT; ++i) m = static_cast<uint16_t>(m | bitAt(i));
  return m;
}
constexpr uint16_t ALL_BITS = allBits();

}  // namespace

uint16_t layoutBit(const freeink::ui::KeyboardLayoutId id) {
  const uint8_t i = indexOf(id);
  return i < COUNT ? bitAt(i) : 0;
}

bool hasLatin(const uint16_t mask) {
  for (uint8_t i = 0; i < COUNT; ++i) {
    if (ALL[i].latin && (mask & bitAt(i))) return true;
  }
  return false;
}

uint16_t enabled() {
  // Drop bits naming no layout: the mask comes from a hand-editable file and
  // survives downgrades, so it can carry bits this build does not have. Left
  // in, such a mask would read as "configured" while enabling nothing.
  const uint16_t configured = static_cast<uint16_t>(SETTINGS.keyboardLayouts & ALL_BITS);
  if (configured != 0) {
    // Same reason English goes back in: a Latin-free set could not type a Wi-Fi
    // passphrase. The settings screen already refuses to produce one.
    if (hasLatin(configured)) return configured;
    return static_cast<uint16_t>(configured | layoutBit(freeink::ui::KeyboardLayoutId::QwertyEn));
  }
  // Unconfigured: the UI language's layout plus English. An English UI collapses
  // to one layout and the language key disappears -- there is nowhere to go.
  return static_cast<uint16_t>(layoutBit(forLanguage(I18N.getLanguage())) |
                               layoutBit(freeink::ui::KeyboardLayoutId::QwertyEn));
}

uint8_t enabledCount() { return static_cast<uint8_t>(std::popcount(enabled())); }

freeink::ui::KeyboardLayoutId startingLayout(const Language language) {
  const freeink::ui::KeyboardLayoutId preferred = forLanguage(language);
  if (enabled() & layoutBit(preferred)) return preferred;
  // Switched off: opening on it anyway would ignore a deliberate choice.
  return next(preferred);
}

freeink::ui::KeyboardLayoutId next(const freeink::ui::KeyboardLayoutId current) {
  const uint16_t mask = enabled();
  const uint8_t from = indexOf(current);
  // A current layout the table does not list still has to lead somewhere.
  const uint8_t start = from < COUNT ? from : 0;
  for (uint8_t step = 1; step <= COUNT; ++step) {
    const uint8_t i = static_cast<uint8_t>((start + step) % COUNT);
    if (mask & bitAt(i)) return ALL[i].id;
  }
  return current;
}

const char* codeFor(const freeink::ui::KeyboardLayoutId id) {
  const uint8_t i = indexOf(id);
  return i < COUNT ? ALL[i].code : "ENG";
}

}  // namespace keyboard_layouts
