#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

// The reader font: an SD-card family name ("" for the built-in family) and a
// point size, plus the spacing that suits it. A book can keep its own; see
// CrossPointSettings::beginBookFont().
// The built-in family index (fontFamily) is not part of it: there is one.
struct ReaderFont {
  char sdFontFamilyName[32] = "";
  uint8_t fontPointSize = 0;
  // Same encoding as the CrossPointSettings fields of the same name.
  uint8_t lineSpacing = 0;
  uint8_t wordSpacing = 0;
  uint8_t characterSpacing = 0;

  void setFamily(const char* name) { snprintf(sdFontFamilyName, sizeof(sdFontFamilyName), "%s", name ? name : ""); }

  bool operator==(const ReaderFont& other) const;
  bool operator!=(const ReaderFont& other) const { return !(*this == other); }
};

// The parts of a ReaderFont a book keeps separately. Family and size go
// together, since the sizes on offer depend on the family.
namespace BookFontField {
constexpr uint8_t Font = 1 << 0;
constexpr uint8_t LineSpacing = 1 << 1;
constexpr uint8_t WordSpacing = 1 << 2;
constexpr uint8_t CharacterSpacing = 1 << 3;
}  // namespace BookFontField

// The BookFontField bits in which a and b differ.
inline uint8_t differingFields(const ReaderFont& a, const ReaderFont& b) {
  uint8_t fields = 0;
  if (a.fontPointSize != b.fontPointSize || strcmp(a.sdFontFamilyName, b.sdFontFamilyName) != 0) {
    fields |= BookFontField::Font;
  }
  if (a.lineSpacing != b.lineSpacing) fields |= BookFontField::LineSpacing;
  if (a.wordSpacing != b.wordSpacing) fields |= BookFontField::WordSpacing;
  if (a.characterSpacing != b.characterSpacing) fields |= BookFontField::CharacterSpacing;
  return fields;
}

inline bool ReaderFont::operator==(const ReaderFont& other) const { return differingFields(*this, other) == 0; }

// Copies the given BookFontField parts of `from` into `to`.
inline void copyFields(ReaderFont& to, const ReaderFont& from, const uint8_t fields) {
  if (fields & BookFontField::Font) {
    memcpy(to.sdFontFamilyName, from.sdFontFamilyName, sizeof(to.sdFontFamilyName));
    to.fontPointSize = from.fontPointSize;
  }
  if (fields & BookFontField::LineSpacing) to.lineSpacing = from.lineSpacing;
  if (fields & BookFontField::WordSpacing) to.wordSpacing = from.wordSpacing;
  if (fields & BookFontField::CharacterSpacing) to.characterSpacing = from.characterSpacing;
}

// What a book keeps: the parts in `fields`, taken from `font`, replace the
// default. A book with no fields follows the default and has no file.
struct BookFont {
  ReaderFont font;
  uint8_t fields = 0;

  bool operator==(const BookFont& other) const {
    return fields == other.fields && (differingFields(font, other.font) & fields) == 0;
  }
  bool operator!=(const BookFont& other) const { return !(*this == other); }
};

enum class BookFontWrite : uint8_t { None, Save, Remove };

// Decides what the book's file should hold after a change, part by part, and
// sets `next` to it.
// Only parts the user changed are written: `inUse` is what the book used when
// it was opened or last saved, so a fallback the reader made on its own when
// opening the book (a family missing from the card, a failed load) leaves the
// file alone.
// A book keeps a part only while it differs from the default, so picking the
// default again makes the book follow later default changes. `saved` is what
// the book's file holds (no fields when it has none).
inline BookFontWrite bookFontWrite(const ReaderFont& current, const ReaderFont& inUse, const ReaderFont& defaultFont,
                                   const BookFont& saved, BookFont& next) {
  const uint8_t changed = differingFields(current, inUse);
  const uint8_t kept = saved.fields & ~changed;
  next = BookFont{};
  next.fields = kept | (changed & differingFields(current, defaultFont));
  copyFields(next.font, saved.font, kept);
  copyFields(next.font, current, next.fields & ~kept);
  if (next == saved) return BookFontWrite::None;
  return next.fields == 0 ? BookFontWrite::Remove : BookFontWrite::Save;
}
