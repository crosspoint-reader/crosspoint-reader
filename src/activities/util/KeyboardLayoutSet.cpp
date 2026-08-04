#include "KeyboardLayoutSet.h"

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

}  // namespace

namespace {
// Two layouts sharing a bit would make one silently toggle the other; a gap is
// harmless (it just means a retired layout), a collision is not.
constexpr bool bitIndicesUnique() {
  for (uint8_t i = 0; i < COUNT; ++i) {
    for (uint8_t j = static_cast<uint8_t>(i + 1); j < COUNT; ++j) {
      if (ALL[i].bitIndex == ALL[j].bitIndex) return false;
    }
  }
  return true;
}
constexpr bool bitIndicesInRange() {
  for (uint8_t i = 0; i < COUNT; ++i) {
    if (ALL[i].bitIndex >= 16) return false;
  }
  return true;
}
}  // namespace

static_assert(bitIndicesUnique(), "two layouts share a persisted bit");
static_assert(bitIndicesInRange(), "a layout's persisted bit does not fit the uint16_t mask");

uint16_t layoutBit(const freeink::ui::KeyboardLayoutId id) {
  const uint8_t i = indexOf(id);
  // An id the table does not list has no persistent bit; returning 0 keeps it
  // out of every mask test rather than aliasing onto someone else's bit.
  return i < COUNT ? static_cast<uint16_t>(1u << ALL[i].bitIndex) : 0;
}

freeink::ui::KeyboardLayoutId forLanguage(const Language language) {
  for (uint8_t i = 0; i < COUNT; ++i) {
    if (ALL[i].language == language) return ALL[i].id;
  }
  return freeink::ui::KeyboardLayoutId::QwertyEn;
}

uint16_t enabled() {
  // Drop bits that name no layout: the mask is user-editable through the
  // settings file and survives firmware downgrades, so it can carry bits for
  // layouts this build does not have. Without the filter such a mask would read
  // as "configured" while enabling nothing, leaving the user with no layouts.
  uint16_t all = 0;
  for (uint8_t i = 0; i < COUNT; ++i) all = static_cast<uint16_t>(all | layoutBit(ALL[i].id));
  const uint16_t configured = static_cast<uint16_t>(SETTINGS.keyboardLayouts & all);
  if (configured != 0) return configured;
  // Unconfigured: the UI language's layout plus English. When the UI is already
  // English this collapses to a single layout and the language key disappears,
  // which is correct -- there is nothing to switch to.
  return static_cast<uint16_t>(layoutBit(forLanguage(I18N.getLanguage())) |
                               layoutBit(freeink::ui::KeyboardLayoutId::QwertyEn));
}

freeink::ui::KeyboardLayoutId startingLayout(const Language language) {
  const freeink::ui::KeyboardLayoutId preferred = forLanguage(language);
  const uint16_t mask = enabled();
  if (mask & layoutBit(preferred)) return preferred;
  // The UI language's layout is switched off. Opening on it anyway would ignore
  // a deliberate choice, so fall back to the first one that is enabled.
  for (uint8_t i = 0; i < COUNT; ++i) {
    if (mask & layoutBit(ALL[i].id)) return ALL[i].id;
  }
  // enabled() never returns an empty set, so this is unreachable in practice.
  return freeink::ui::KeyboardLayoutId::QwertyEn;
}

uint8_t enabledCount() {
  const uint16_t mask = enabled();
  uint8_t n = 0;
  for (uint8_t i = 0; i < COUNT; ++i) {
    if (mask & layoutBit(ALL[i].id)) ++n;
  }
  return n;
}

freeink::ui::KeyboardLayoutId next(const freeink::ui::KeyboardLayoutId current) {
  const uint16_t mask = enabled();
  const uint8_t from = indexOf(current);
  // An unlisted current layout (or one the user has since disabled) still has to
  // lead somewhere: start the scan at the top of the table.
  const uint8_t start = from < COUNT ? from : 0;
  for (uint8_t step = 1; step <= COUNT; ++step) {
    const uint8_t i = static_cast<uint8_t>((start + step) % COUNT);
    if (mask & layoutBit(ALL[i].id)) return ALL[i].id;
  }
  return current;
}

const char* codeFor(const freeink::ui::KeyboardLayoutId id) {
  const uint8_t i = indexOf(id);
  // Fallback matches ALL[0], which is English -- ISO 639-3 like the rest.
  return i < COUNT ? ALL[i].code : "ENG";
}

}  // namespace keyboard_layouts
