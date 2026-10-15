#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace ttf_test {

/// One glyph as FreeType renders it unhinted.
struct ReferenceGlyph {
  bool loaded = false;
  bool overlap = false;  ///< the outline carries an OVERLAP flag: FreeType renders it 4x4 oversampled
  int width = 0;
  int height = 0;
  int left = 0;
  int top = 0;
  int32_t linearAdvance = 0;    ///< linearHoriAdvance, 16.16
  std::vector<uint8_t> bitmap;  ///< packed 2-bit, as fontconvert_sdcard.py packs it
};

/// FreeType 2.13.3, the SDK's copy, set up as the PC converter sets it up (FT_Set_Char_Size at 150 dpi) but
/// loading unhinted: the reference the TTF engine is checked against. The only test file that includes
/// FreeType's headers; the engine's ftimage.h would clash with them.
class FreeTypeReference {
 public:
  FreeTypeReference(const std::vector<uint8_t>& font, uint32_t points);
  ~FreeTypeReference();
  FreeTypeReference(const FreeTypeReference&) = delete;
  FreeTypeReference& operator=(const FreeTypeReference&) = delete;

  bool ok() const;
  uint32_t numGlyphs() const;
  uint32_t glyphIndex(uint32_t codepoint) const;
  /// The glyph unhinted. FreeType renders an outline flagged OVERLAP 4x4 oversampled, which the engine does
  /// not (spec 11.6); unless `oversampleOverlaps`, the flag is cleared first, so every glyph renders the way
  /// the engine does.
  ReferenceGlyph glyph(uint32_t index, bool oversampleOverlaps = false) const;
  /// The size metrics in 26.6, as the converter reads face.size.
  int32_t ascender() const;
  int32_t descender() const;
  int32_t height() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ttf_test
