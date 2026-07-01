#pragma once
#include <cstdint>
#include <string>

// Lightweight Arabic character shaper for the CrossPoint Reader.
//
// Applies Unicode Presentation Forms (U+FB50-U+FDFF, U+FE70-U+FEFF) to Arabic
// text in logical order, replacing base codepoints with their contextual
// isolated / initial / medial / final forms.  Must be called BEFORE BiDi
// reordering so the UAX#9 algorithm operates on already-shaped text.
//
// Requirements:
//   The font must include the Arabic Presentation Forms ranges.  Convert with
//   the "arabic" interval preset in fontconvert_sdcard.py.

namespace ArabicShaper {

// Shape Arabic text in logical order.
//
// Returns `text` unchanged when no shapeable Arabic is found (fast path,
// no allocation).  Otherwise writes shaped UTF-8 to `shaped` and returns
// `shaped.c_str()`.  The caller must keep `shaped` alive while the returned
// pointer is in use.
const char* shape(const char* text, std::string& shaped);

}  // namespace ArabicShaper
