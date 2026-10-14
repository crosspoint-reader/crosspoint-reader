#include "FreeTypeReference.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

namespace ttf_test {

struct FreeTypeReference::Impl {
  FT_Library library = nullptr;
  FT_Face face = nullptr;
};

FreeTypeReference::FreeTypeReference(const std::vector<uint8_t>& font, const uint32_t points) : impl_(new Impl) {
  if (FT_Init_FreeType(&impl_->library) != 0) return;
  if (FT_New_Memory_Face(impl_->library, font.data(), static_cast<FT_Long>(font.size()), 0, &impl_->face) != 0) {
    impl_->face = nullptr;
    return;
  }
  const FT_F26Dot6 size = static_cast<FT_F26Dot6>(points) << 6;
  if (FT_Set_Char_Size(impl_->face, size, size, 150, 150) != 0) {
    FT_Done_Face(impl_->face);
    impl_->face = nullptr;
  }
}

FreeTypeReference::~FreeTypeReference() {
  if (impl_->face) FT_Done_Face(impl_->face);
  if (impl_->library) FT_Done_FreeType(impl_->library);
}

bool FreeTypeReference::ok() const { return impl_->face != nullptr; }
uint32_t FreeTypeReference::numGlyphs() const { return static_cast<uint32_t>(impl_->face->num_glyphs); }
uint32_t FreeTypeReference::glyphIndex(const uint32_t codepoint) const {
  return FT_Get_Char_Index(impl_->face, codepoint);
}
int32_t FreeTypeReference::ascender() const { return static_cast<int32_t>(impl_->face->size->metrics.ascender); }
int32_t FreeTypeReference::descender() const { return static_cast<int32_t>(impl_->face->size->metrics.descender); }
int32_t FreeTypeReference::height() const { return static_cast<int32_t>(impl_->face->size->metrics.height); }

ReferenceGlyph FreeTypeReference::glyph(const uint32_t index, const bool oversampleOverlaps) const {
  ReferenceGlyph out;
  FT_GlyphSlot slot = impl_->face->glyph;
  if (FT_Load_Glyph(impl_->face, index, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) return out;
  out.overlap = (slot->outline.flags & FT_OUTLINE_OVERLAP) != 0;
  if (!oversampleOverlaps) slot->outline.flags &= ~FT_OUTLINE_OVERLAP;
  out.linearAdvance = static_cast<int32_t>(slot->linearHoriAdvance);
  if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) return out;
  out.loaded = true;
  const FT_Bitmap& bitmap = slot->bitmap;
  out.width = static_cast<int>(bitmap.width);
  out.height = static_cast<int>(bitmap.rows);
  out.left = slot->bitmap_left;
  out.top = slot->bitmap_top;
  out.bitmap.assign((out.width * out.height + 3) / 4, 0);
  for (int y = 0; y < out.height; ++y) {
    const unsigned char* row = bitmap.buffer + y * bitmap.pitch;
    for (int x = 0; x < out.width; ++x) {
      const int level = row[x] >> 4;
      const int value = level >= 12 ? 3 : level >= 8 ? 2 : level >= 4 ? 1 : 0;
      const int p = y * out.width + x;
      out.bitmap[p >> 2] |= static_cast<uint8_t>(value << (6 - 2 * (p & 3)));
    }
  }
  return out;
}

}  // namespace ttf_test
