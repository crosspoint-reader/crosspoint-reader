#pragma once

#include <cstdint>

#include "EpdFontData.h"
#include "GlyphProvider.h"

/// Serves a font's missing glyphs out of other BUILT-IN faces: the CJK UI faces behind the Inter UI
/// faces (src/CjkUiFonts.cpp).
///
/// Installed as EpdFontData::provider, so EpdFont asks it only after the font's own interval table
/// has missed, and the renderer gets the bitmap back through ownsBitmap(). Kerning stays the font's
/// own: a codepoint the font has no glyph for is in none of its kern classes.
///
/// The faces must be uncompressed, so a served glyph's bitmap is a plain pointer into flash, and of
/// the bit depth of the font asking, because the renderer draws at the depth of the font it was
/// given. setFaces() refuses a compressed face; the depth is the caller's to match.
class FallbackFaceProvider final : public GlyphProvider {
 public:
  static constexpr uint8_t MAX_FACES = 4;

  /// The faces to search, first match wins. False, and no faces, when there are too many or one of
  /// them is compressed.
  bool setFaces(const EpdFontData* const* faces, uint8_t count);

  /// A codepoint `claims` returns true for that no face has is served as `standIn` instead, from
  /// the first face that has that. Subset faces carry only part of their script, and a character
  /// of it they lack should show as missing rather than vanish from the middle of a word. Codepoints
  /// outside the claim stay missing, as for any font.
  void setStandIn(uint32_t standIn, bool (*claims)(uint32_t codepoint)) {
    standIn_ = standIn;
    claims_ = claims;
  }

  const EpdGlyph* missGlyph(uint32_t codepoint) override;
  bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const override;

 private:
  // A served glyph as GlyphProvider hands it out: a full EpdGlyph record (the faces store the 6-byte
  // packed form, which has no field to say whose glyph it is) and where its bitmap is. `record`
  // comes first so ownsBitmap() can get from the record pointer back to the slot.
  struct Served {
    EpdGlyph record;
    const uint8_t* bitmap;
  };
  // EpdFont copies the metrics out at once and the renderer asks for the bitmap before it resolves
  // the next glyph, so one slot would do for that caller. The few more cost 20 B each and keep a
  // caller that resolves a glyph or two ahead correct, the same lifetime SdCardFont's miss ring
  // gives (GlyphProvider::missGlyph).
  static constexpr uint8_t RING_SIZE = 8;
  Served ring_[RING_SIZE] = {};
  uint8_t next_ = 0;
  const EpdFontData* faces_[MAX_FACES] = {};
  uint8_t faceCount_ = 0;
  uint32_t standIn_ = 0;
  bool (*claims_)(uint32_t) = nullptr;

  const EpdGlyph* serve(uint32_t codepoint);
};
