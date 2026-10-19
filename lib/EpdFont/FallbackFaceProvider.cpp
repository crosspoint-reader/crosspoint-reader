#include "FallbackFaceProvider.h"

#include "EpdFont.h"
#include "FontDecompressor.h"

bool FallbackFaceProvider::setFaces(const EpdFontData* const* faces, const uint8_t count) {
  faceCount_ = 0;
  if (count > MAX_FACES) return false;
  for (uint8_t i = 0; i < count; ++i) {
    if (faces[i]->groups && !decompressor_) return false;
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
    // No bitmap is touched here: measuring text resolves glyphs by the hundred and never draws them,
    // so a compressed face pays its decode only in ownsBitmap(), for a glyph actually drawn.
    // dataOffset is the face's own for a flat face and 0 for a compressed one, which has no flat
    // array; nothing reads it for a provider record, whose bitmap comes from ownsBitmap().
    slot.record = EpdGlyph{glyph.width,
                           glyph.height,
                           glyph.advanceX,
                           glyph.left,
                           glyph.top,
                           glyphDataBytes(glyph.width, glyph.height, face->is2Bit),
                           face->groups ? 0 : epdGlyphBitmapOffset(face, glyph)};
    slot.face = face;
    slot.index = glyph.index;
    return &slot.record;
  }
  return nullptr;
}

bool FallbackFaceProvider::ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const {
  for (const Served& slot : ring_) {
    if (&slot.record != glyph) continue;
    const EpdGlyph& r = slot.record;
    bitmap = nullptr;
    if (r.width == 0 || r.height == 0 || !slot.face) return true;
    if (!slot.face->groups) {
      bitmap = slot.face->bitmap + r.dataOffset;
      return true;
    }
    // setFaces() admits a compressed face only with a decompressor. The pointer is the
    // decompressor's fallback cache slot: valid until its next decode, and the renderer draws it
    // before resolving another glyph.
    const EpdGlyphRef ref{
        nullptr, r.advanceX, slot.index, r.width, r.height, static_cast<int8_t>(r.left), static_cast<int8_t>(r.top),
        true};
    if (decompressor_) bitmap = decompressor_->getBitmap(slot.face, ref, slot.index);
    return true;
  }
  return false;
}
