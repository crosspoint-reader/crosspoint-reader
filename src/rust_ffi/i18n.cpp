// Translation lookup.

#include <I18n.h>

#include "internal.h"

extern "C" {

// Returns the key itself when unknown, so a typo shows on screen rather than
// crashing.
const uint8_t* cpp_tr(const uint8_t* key) {
  return reinterpret_cast<const uint8_t*>(I18N.getByKey(key ? rust_ffi::asText(key) : nullptr));
}
}
