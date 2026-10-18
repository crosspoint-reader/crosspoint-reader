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
// block takes one family, and a generic serif or sans-serif maps onto a built-in face.
struct BlockFontFamilies {
  // The built-in faces for the two generic families and the book's accent family: beside a TrueType reader font
  // neither generic is the body's own.
  static constexpr int kMaxFamilies = 3;

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

  // The family a block whose font-family is `cssFamily` (CssStyle::fontFamily) is laid out in: its named family
  // when that is here, else its generic one; nullptr for the body font.
  const Family* find(const uint32_t cssFamily) const {
    const uint32_t identity = CssFontFamily::identity(cssFamily);
    if (identity != 0 && identity == CssFontFamily::identity(body)) return nullptr;
    for (const uint32_t key : {identity, cssFamily & CssFontFamily::kGenericMask}) {
      for (int i = 0; key != 0 && i < count; ++i) {
        if (families[i].key == key) return &families[i];
      }
    }
    return nullptr;
  }
};
