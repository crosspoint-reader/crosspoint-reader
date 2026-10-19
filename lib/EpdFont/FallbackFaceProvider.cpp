#include "FallbackFaceProvider.h"

#include "EpdFont.h"

bool FallbackFaceProvider::setFaces(const EpdFontData* const* faces, const uint8_t count) {
  faceCount_ = 0;
  if (count > MAX_FACES) return false;
  for (uint8_t i = 0; i < count; ++i) {
    if (faces[i]->groups) return false;
  }
  for (uint8_t i = 0; i < count; ++i) faces_[i] = faces[i];
  faceCount_ = count;
  return true;
}

const EpdGlyph* FallbackFaceProvider::missGlyph(const uint32_t codepoint) {
  if (const EpdGlyph* glyph = serve(codepoint)) return glyph;
  if (claims_ && codepoint != standIn_ && claims_(codepoint)) return serve(standIn_);
  return nullptr;
}

const EpdGlyph* FallbackFaceProvider::serve(const uint32_t codepoint) {
  for (uint8_t i = 0; i < faceCount_; ++i) {
    const EpdFontData* face = faces_[i];
    const EpdGlyphRef glyph = EpdFont(face).findGlyph(codepoint);
    if (!glyph) continue;

    Served& slot = ring_[next_];
    next_ = static_cast<uint8_t>((next_ + 1) % RING_SIZE);
    const uint32_t offset = epdGlyphBitmapOffset(face, glyph);
    slot.record = EpdGlyph{glyph.width, glyph.height, glyph.advanceX,
                           glyph.left,  glyph.top,    glyphDataBytes(glyph.width, glyph.height, face->is2Bit),
                           offset};
    slot.bitmap = (glyph.width && glyph.height) ? face->bitmap + offset : nullptr;
    return &slot.record;
  }
  return nullptr;
}

bool FallbackFaceProvider::ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const {
  for (const Served& slot : ring_) {
    if (&slot.record == glyph) {
      bitmap = slot.bitmap;
      return true;
    }
  }
  return false;
}
