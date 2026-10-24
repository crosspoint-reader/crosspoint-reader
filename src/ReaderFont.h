#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

// The reader font: an SD-card family name ("" for the built-in family) and a
// point size. A book can keep its own; see CrossPointSettings::beginBookFont().
// The built-in family index (fontFamily) is not part of it: there is one.
struct ReaderFont {
  char sdFontFamilyName[32] = "";
  uint8_t fontPointSize = 0;

  void setFamily(const char* name) { snprintf(sdFontFamilyName, sizeof(sdFontFamilyName), "%s", name ? name : ""); }

  bool operator==(const ReaderFont& other) const {
    return fontPointSize == other.fontPointSize && strcmp(sdFontFamilyName, other.sdFontFamilyName) == 0;
  }
  bool operator!=(const ReaderFont& other) const { return !(*this == other); }
};

enum class BookFontWrite : uint8_t { None, Save, Remove };

// Only a font the user picked is written: `inUse` is the font the book used
// when it was opened or last saved, so a fallback the reader made on its own
// when opening the book (a family missing from the card, a failed load) leaves
// the file alone.
// A book keeps its own font only while it differs from the default, so picking
// the default again makes the book follow later default changes. `saved` is the
// font in the book's file, or nullptr when it has none.
inline BookFontWrite bookFontWrite(const ReaderFont& current, const ReaderFont& inUse, const ReaderFont& defaultFont,
                                   const ReaderFont* saved) {
  if (current == inUse) return BookFontWrite::None;
  if (current == defaultFont) return saved ? BookFontWrite::Remove : BookFontWrite::None;
  if (saved && *saved == current) return BookFontWrite::None;
  return BookFontWrite::Save;
}
