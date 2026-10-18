#pragma once

#include <cstdint>

#include "FontSizeLadder.h"
#include "css/CssStyle.h"

// A book's other font families (the Publisher Fonts modes Accents and All Text): a block whose CSS font-family is one
// of them is laid out in that family's face at the block's size, through the block font ID, as a heading takes a
// rung of the body's ladder (ChapterHtmlSlimParser::resolveBlockFont). The app builds it, as it builds the ladder: it
// knows the faces. Empty (no families) lays a book out as before; a section is keyed on what it holds otherwise
// (Section::calculatePropertyHash).
//
// The idea of per-element font slots for a book's families is crosspoint-reader PR #3934's (Justin Mitchell); here a
// block takes one family, a word in another family than its block's that family's face through a font slot of its
// own (TextBlock::WordFont), and a generic serif or sans-serif maps onto a built-in face.
struct BlockFontFamilies {
  // The built-in faces for the two generic families and the book's accent families (BookFonts::kMaxAccents): beside
  // a TrueType reader font neither generic is the body's own.
  static constexpr int kMaxFamilies = 5;

  struct Family {
    // A family's identity (CssFontFamily::identity), or a generic family (CssFontFamily::kSerif, kSansSerif).
    uint32_t key = 0;
    int32_t bodyFontId = 0;  // its face at the body size; a full 32-bit font ID
    FontSizeLadder ladder;   // its sizes, as percents of the body size, or its native sizes
  };

  Family families[kMaxFamilies] = {};
  uint8_t count = 0;
  // The body text's font-family (CssStyle::fontFamily): a block in it keeps the body font, whichever generic
  // family its list falls back to.
  uint32_t body = 0;

  void add(const uint32_t key, const int32_t bodyFontId, const FontSizeLadder& ladder) {
    if (count >= kMaxFamilies || key == 0 || bodyFontId == 0) return;
    families[count++] = {key, bodyFontId, ladder};
  }

  // The family text whose font-family is `cssFamily` (CssStyle::fontFamily) is laid out in, as a slot: its named
  // family when that is here, else its generic one, as its index plus one; 0 for the body font. A block takes one
  // family; a word in another than its block's has a font slot of this number (ParsedText::addWord).
  uint8_t slotOf(const uint32_t cssFamily) const {
    const uint32_t identity = CssFontFamily::identity(cssFamily);
    if (identity != 0 && identity == CssFontFamily::identity(body)) return 0;
    for (const uint32_t key : {identity, cssFamily & CssFontFamily::kGenericMask}) {
      for (int i = 0; key != 0 && i < count; ++i) {
        if (families[i].key == key) return static_cast<uint8_t>(i + 1);
      }
    }
    return 0;
  }
  // The family of a slot slotOf() gave; nullptr for the body font.
  const Family* family(const uint8_t slot) const { return slot == 0 || slot > count ? nullptr : &families[slot - 1]; }
};
