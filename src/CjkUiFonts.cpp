#include "CjkUiFonts.h"

#include <EpdFont.h>
#include <FallbackFaceProvider.h>
#include <FlashLangPartition.h>
#include <FontDecompressor.h>
#include <Logging.h>
#include <SerialTransfer/SerialTransferProtocol.h>
#include <Utf8.h>
#include <builtinFonts/cjk_ui_ja_10.h>
#include <builtinFonts/cjk_ui_ja_12.h>
#include <builtinFonts/cjk_ui_ko_10.h>
#include <builtinFonts/cjk_ui_ko_12.h>
#include <builtinFonts/cjk_ui_zh_10.h>
#include <builtinFonts/cjk_ui_zh_12.h>

#include <cstddef>

namespace {

// CJK_UI_FONT_SIZES in lib/EpdFont/scripts/font-sizes.sh, ascending; a size added there needs its
// faces added to every row below.
constexpr uint8_t SIZES[] = {10, 12};
constexpr size_t SIZE_COUNT = sizeof(SIZES) / sizeof(SIZES[0]);

// A compressed face, and the flat bitmap it inflates to (compress_ui_face.py states both).
struct Face {
  const EpdFontData* data;
  uint32_t packedBytes;
  uint32_t packedCrc32;
};

struct LanguageFaces {
  Language language;
  Face faces[SIZE_COUNT];
};

// In the order searched for a non-CJK UI language. The faces and their regions are listed in
// lib/EpdFont/scripts/ui_font_coverage.py (CJK_UI_LANGUAGES).
const LanguageFaces LANGUAGES[] = {
    {Language::ZH,
     {{&cjk_ui_zh_10, cjk_ui_zh_10PackedBytes, cjk_ui_zh_10PackedCrc32},
      {&cjk_ui_zh_12, cjk_ui_zh_12PackedBytes, cjk_ui_zh_12PackedCrc32}}},
    {Language::JA,
     {{&cjk_ui_ja_10, cjk_ui_ja_10PackedBytes, cjk_ui_ja_10PackedCrc32},
      {&cjk_ui_ja_12, cjk_ui_ja_12PackedBytes, cjk_ui_ja_12PackedCrc32}}},
    {Language::KO,
     {{&cjk_ui_ko_10, cjk_ui_ko_10PackedBytes, cjk_ui_ko_10PackedCrc32},
      {&cjk_ui_ko_12, cjk_ui_ko_12PackedBytes, cjk_ui_ko_12PackedCrc32}}},
};
constexpr size_t LANGUAGE_COUNT = sizeof(LANGUAGES) / sizeof(LANGUAGES[0]);
static_assert(LANGUAGE_COUNT <= FallbackFaceProvider::MAX_FACES, "one provider face per CJK language");
// The faces in the slot index their bitmaps with uint16_t offsets, as an uncompressed face does.
static_assert(cjk_ui_zh_10PackedBytes <= 0xFFFF && cjk_ui_zh_12PackedBytes <= 0xFFFF &&
                  cjk_ui_ja_10PackedBytes <= 0xFFFF && cjk_ui_ja_12PackedBytes <= 0xFFFF &&
                  cjk_ui_ko_10PackedBytes <= 0xFFFF && cjk_ui_ko_12PackedBytes <= 0xFFFF,
              "a CJK UI face past 64 KB of bitmap needs uint32_t offsets in the language pack slot");

FallbackFaceProvider providers[SIZE_COUNT];

// WHITE SQUARE, which every CJK UI face carries (CJK_UI_STAND_IN in ui_font_coverage.py): what a
// CJK character no face has draws as. A book title the faces do not cover then shows boxes where
// its characters are, instead of the one or two it shares with a translation standing alone.
constexpr uint32_t STAND_IN = 0x25A1;

// The characters the CJK faces answer for. Not the ideographic space, which every face carries
// anyway (a box is no stand-in for a space), and not the Hangul fillers, which are default-ignorable
// and must never draw ink.
bool isCjkCharacter(const uint32_t cp) { return utf8IsCjkBreakable(cp) && cp != 0x3000 && !utf8IsDefaultIgnorable(cp); }

// The UI faces main.cpp attaches: notosans_8 and Inter 10/12/14 in regular and bold.
constexpr size_t MAX_ATTACHED = 7;
EpdFontData attached[MAX_ATTACHED];
size_t attachedCount = 0;

size_t sizeIndexFor(const uint8_t points) {
  size_t index = 0;
  for (size_t i = 0; i < SIZE_COUNT; ++i) {
    if (SIZES[i] <= points) index = i;
  }
  return index;
}

const LanguageFaces* facesOf(const Language language) {
  for (const LanguageFaces& row : LANGUAGES) {
    if (row.language == language) return &row;
  }
  return nullptr;
}

// --- The language pack extension: the active language's faces, inflated into the slot ---
//
// Layout, per size in SIZES order, each part 4-byte aligned:
//   [flat bitmap, packedBytes][bitmap offsets, glyphCount x uint16_t]
// exactly the two arrays an uncompressed face carries, so a RAM copy of the face pointed at them
// draws as one.

constexpr uint32_t align4(const uint32_t v) { return (v + 3u) & ~3u; }

uint32_t glyphCountOf(const EpdFontData* face) {
  const EpdFontGroup& last = face->groups[face->groupCount - 1];
  return last.firstGlyphIndex + last.glyphCount;
}

uint32_t faceBytes(const Face& face) { return align4(face.packedBytes) + align4(2 * glyphCountOf(face.data)); }

// RAM copies of the active language's faces, pointing into the mapped slot: what the providers
// serve that language from. Two EpdFontData, ~240 B of .bss -- the only way to point a face whose
// table is const flash at a bitmap that lives elsewhere, as for the seven attached UI faces above.
EpdFontData inSlot[SIZE_COUNT];
Language inSlotLanguage = Language::_COUNT;  // none

// setLanguage() has run, so the providers may be (re)ordered; until then attach() only records.
bool languageSet = false;
Language uiLanguage = Language::EN;

const EpdFontData* servedFace(const LanguageFaces& row, const size_t size) {
  return row.language == inSlotLanguage ? &inSlot[size] : row.faces[size].data;
}

void orderFaces() {
  for (size_t size = 0; size < SIZE_COUNT; ++size) {
    const EpdFontData* order[LANGUAGE_COUNT];
    size_t count = 0;
    for (const LanguageFaces& row : LANGUAGES) {
      if (row.language == uiLanguage) order[count++] = servedFace(row, size);
    }
    for (const LanguageFaces& row : LANGUAGES) {
      if (row.language != uiLanguage) order[count++] = servedFace(row, size);
    }
    if (!providers[size].setFaces(order, static_cast<uint8_t>(count))) {
      LOG_ERR("CJK", "CJK UI faces at %u pt refused", SIZES[size]);
    }
    providers[size].setStandIn(STAND_IN, &isCjkCharacter);
  }
}

// Feeds inflated glyphs to the slot, checking them against the CRC the generator recorded: a face
// that does not inflate to its own bitmap is never served from flash.
struct GlyphSink {
  FlashLangPartition::SlotWriter& out;
  uint32_t crc = 0;
  uint32_t bytes = 0;
};

bool sinkGlyph(void* ctx, const uint8_t* data, const uint32_t len) {
  auto* sink = static_cast<GlyphSink*>(ctx);
  sink->crc = serialtransfer::crc32Update(sink->crc, data, len);
  sink->bytes += len;
  return sink->out.write(data, len);
}

bool pad(FlashLangPartition::SlotWriter& out, const uint32_t written) {
  static constexpr uint8_t ZEROS[3] = {};
  return out.write(ZEROS, align4(written) - written);
}

class PackFaces final : public LanguagePackExtension {
 public:
  uint32_t bytes(const Language lang) override {
    const LanguageFaces* row = facesOf(lang);
    if (!row) return 0;
    uint32_t total = 0;
    for (const Face& face : row->faces) total += faceBytes(face);
    return total;
  }

  uint32_t stamp(const Language lang) override {
    const LanguageFaces* row = facesOf(lang);
    if (!row) return 0;
    // The bitmaps' CRCs identify the content; the rotation keeps two faces with swapped CRCs apart.
    uint32_t s = 0x43464b55;  // "UKFC"
    for (const Face& face : row->faces) s = ((s << 7) | (s >> 25)) ^ face.packedCrc32 ^ face.packedBytes;
    return s;
  }

  bool produce(const Language lang, FlashLangPartition::SlotWriter& out) override {
    const LanguageFaces* row = facesOf(lang);
    if (!row) return false;
    // The inflate ring and the one-glyph buffer, both out of the slot builder's spare ring: the
    // strings were unpacked through it and are done. No allocation here.
    uint8_t* scratch = out.scratch();
    const uint32_t glyphBufAt = align4(FontDecompressor::HOT_GLYPH_BUF_SIZE);
    for (const Face& face : row->faces) {
      const uint32_t ring = FontDecompressor::maxRingBytes(face.data);
      if (glyphBufAt + ring > out.scratchBytes()) {
        LOG_ERR("CJK", "face needs a %u B ring, the slot builder lends %u B", static_cast<unsigned>(ring),
                static_cast<unsigned>(out.scratchBytes() - glyphBufAt));
        return false;
      }
      GlyphSink sink{out};
      if (!FontDecompressor::inflateFace(face.data, scratch + glyphBufAt, ring, scratch, &sinkGlyph, &sink)) {
        LOG_ERR("CJK", "face inflate failed after %u B", static_cast<unsigned>(sink.bytes));
        return false;
      }
      if (sink.bytes != face.packedBytes || sink.crc != face.packedCrc32) {
        LOG_ERR("CJK", "face inflated to %u B, CRC %08lx; expected %u B, CRC %08lx", static_cast<unsigned>(sink.bytes),
                static_cast<unsigned long>(sink.crc), static_cast<unsigned>(face.packedBytes),
                static_cast<unsigned long>(face.packedCrc32));
        return false;
      }
      if (!pad(out, sink.bytes)) return false;

      // The offsets the uncompressed face carried: the running sum of the glyph sizes. Batched so
      // the writer is not called per glyph.
      uint16_t batch[32];
      uint32_t inBatch = 0;
      uint32_t offset = 0;
      const uint32_t count = glyphCountOf(face.data);
      for (uint32_t i = 0; i < count; ++i) {
        batch[inBatch++] = static_cast<uint16_t>(offset);
        const EpdGlyphRef glyph = epdResolveGlyph(face.data, i);
        offset += glyphDataBytes(glyph.width, glyph.height, false);
        if (inBatch == 32 || i + 1 == count) {
          if (!out.write(batch, inBatch * sizeof(uint16_t))) return false;
          inBatch = 0;
        }
      }
      if (!pad(out, 2 * count)) return false;
    }
    return true;
  }

  void attach(const Language lang, const uint8_t* data, const uint32_t len) override {
    inSlotLanguage = Language::_COUNT;
    const LanguageFaces* row = facesOf(lang);
    if (data && row && len == bytes(lang)) {
      uint32_t at = 0;
      for (size_t size = 0; size < SIZE_COUNT; ++size) {
        const Face& face = row->faces[size];
        EpdFontData& copy = inSlot[size];
        copy = *face.data;
        copy.bitmap = data + at;
        copy.bitmapOffsets = reinterpret_cast<const uint16_t*>(data + at + align4(face.packedBytes));
        copy.groups = nullptr;
        copy.groupCount = 0;
        at += faceBytes(face);
      }
      inSlotLanguage = lang;
      LOG_INF("CJK", "CJK UI faces of language %u read from the language pack", static_cast<unsigned>(lang));
    }
    // Before setLanguage() has run at boot there is nothing to reorder yet: it orders them itself.
    if (languageSet) orderFaces();
  }
};

PackFaces packFaces;

}  // namespace

LanguagePackExtension& CjkUiFonts::packExtension() { return packFaces; }

void CjkUiFonts::setDecompressor(FontDecompressor* decompressor) {
  for (FallbackFaceProvider& provider : providers) provider.setDecompressor(decompressor);
}

void CjkUiFonts::attach(EpdFont& font, const uint8_t points) {
  const EpdFontData* data = font.data;
  // A provider already there is an SD or TTF font's own; a compressed or 2-bit face could not draw
  // a 1-bit glyph from a flat bitmap.
  if (data->provider || data->groups || data->is2Bit) {
    LOG_ERR("CJK", "UI face %u pt cannot take CJK fallback glyphs", points);
    return;
  }
  if (attachedCount == MAX_ATTACHED) {
    LOG_ERR("CJK", "more than %u UI faces attached", static_cast<unsigned>(MAX_ATTACHED));
    return;
  }
  EpdFontData& copy = attached[attachedCount++];
  copy = *data;
  copy.provider = &providers[sizeIndexFor(points)];
  font.data = &copy;
}

void CjkUiFonts::setLanguage(const Language language) {
  uiLanguage = language;
  languageSet = true;
  orderFaces();
}
