// Broken fonts (spec 9.3): cut short anywhere, or with random bytes in any table, a font never crashes the
// engine or makes it read outside the file or its scratch block. CI runs this under AddressSanitizer and
// UBSan (see CMakeLists.txt); elsewhere UBSan traps.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "TtfBuilder.h"
#include "TtfRasterizer.h"
#include "TtfTestUtil.h"

using namespace ttf;
using namespace ttf_test;

namespace {

constexpr uint32_t kPpems[] = {ppemFromPoints(12), ppemFromPoints(40)};

// Parses `font` and, when that works, renders a spread of its glyphs at two sizes. Returns how many rendered.
int exercise(const std::vector<uint8_t>& font, Rasterizer& rasterizer) {
  FontBytes bytes(font.data(), static_cast<uint32_t>(font.size()));
  Face face;
  if (parseFace(bytes, &face) != FaceStatus::Ok) return 0;
  int rendered = 0;
  for (uint32_t cp = 0x20; cp < 0x250; cp += 7) {
    const uint16_t glyph = glyphIndex(bytes, face, cp);
    for (const uint32_t ppem : kPpems) {
      EpdGlyph metrics{};
      const Status status = rasterizer.render(bytes, face, glyph, ppem, &metrics);
      EXPECT_TRUE(status == Status::Ok || isPermanent(status)) << statusName(status);
      rendered += status == Status::Ok;
    }
  }
  return rendered;
}

// The table directory's entries, to aim the mutations.
struct TableSpan {
  uint32_t offset;
  uint32_t length;
};

std::vector<TableSpan> tables(const std::vector<uint8_t>& font) {
  std::vector<TableSpan> out;
  const uint32_t count = static_cast<uint32_t>(font[4] << 8 | font[5]);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* r = font.data() + 12 + 16 * i;
    out.push_back({static_cast<uint32_t>(r[8] << 24 | r[9] << 16 | r[10] << 8 | r[11]),
                   static_cast<uint32_t>(r[12] << 24 | r[13] << 16 | r[14] << 8 | r[15])});
  }
  return out;
}

// A small linear congruential generator: the same mutations on every run and every platform.
struct Random {
  uint32_t state;
  uint32_t next() {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  }
  uint32_t below(const uint32_t n) { return n ? next() % n : 0; }
};

Rasterizer& sharedRasterizer() {
  static Rasterizer rasterizer;
  if (!rasterizer.allocated()) {
    rasterizer.allocate({kMaxScratchPoints, kMaxScratchContours, kMaxOutputBytes, kPoolBytesFlash});
  }
  return rasterizer;
}

}  // namespace

TEST(TtfCorruptFont, TheIntactFontRenders) {
  EXPECT_GT(exercise(readFont("Bookerly/Bookerly-Regular.ttf"), sharedRasterizer()), 100);
}

TEST(TtfCorruptFont, AFontCutShortAnywhereIsSafe) {
  const std::vector<uint8_t> font = readFont("Bookerly/Bookerly-Regular.ttf");
  std::vector<uint32_t> cuts = {0, 1, 4, 11, 12, 13, 100, 200};
  for (const TableSpan& t : tables(font)) {
    for (const uint32_t at : {t.offset, t.offset + 1, t.offset + t.length / 2, t.offset + t.length - 1}) {
      cuts.push_back(at);
    }
  }
  cuts.push_back(static_cast<uint32_t>(font.size() - 1));
  for (const uint32_t cut : cuts) {
    if (cut > font.size()) continue;
    const std::vector<uint8_t> shortened(font.begin(), font.begin() + cut);
    exercise(shortened, sharedRasterizer());
  }
}

TEST(TtfCorruptFont, RandomBytesInAnyTableAreSafe) {
  const std::vector<std::string> sources = {"Bookerly/Bookerly-Regular.ttf", "NotoSans/NotoSans-Italic.ttf",
                                            "Inter/Inter-Bold.ttf"};
  Random random{20261014};
  for (const std::string& source : sources) {
    const std::vector<uint8_t> original = readFont(source);
    const std::vector<TableSpan> spans = tables(original);
    for (int round = 0; round < 150; ++round) {
      std::vector<uint8_t> font = original;
      const TableSpan& t = spans[random.below(static_cast<uint32_t>(spans.size()))];
      const uint32_t changes = 1 + random.below(8);
      for (uint32_t i = 0; i < changes && t.length > 0; ++i) {
        font[t.offset + random.below(t.length)] = static_cast<uint8_t>(random.next());
      }
      exercise(font, sharedRasterizer());
    }
  }
}

TEST(TtfCorruptFont, RandomBytesInTheDirectoryAreSafe) {
  const std::vector<uint8_t> original = readFont("Bookerly/Bookerly-Regular.ttf");
  Random random{7};
  const uint32_t directoryEnd = 12 + 16 * static_cast<uint32_t>(original[4] << 8 | original[5]);
  for (int round = 0; round < 300; ++round) {
    std::vector<uint8_t> font = original;
    font[random.below(directoryEnd)] = static_cast<uint8_t>(random.next());
    exercise(font, sharedRasterizer());
  }
}

namespace {

uint32_t be32(const std::vector<uint8_t>& font, const size_t at) {
  return static_cast<uint32_t>(font[at] << 24 | font[at + 1] << 16 | font[at + 2] << 8 | font[at + 3]);
}

// Where a glyph's record lies in glyf, from the intact face's loca.
void glyphRecord(FontBytes& bytes, const Face& face, const uint16_t glyph, uint32_t* start, uint32_t* end) {
  if (face.longLoca) {
    *start = bytes.u32(face.loca + 4u * glyph);
    *end = bytes.u32(face.loca + 4u * glyph + 4);
  } else {
    *start = 2u * bytes.u16(face.loca + 2u * glyph);
    *end = 2u * bytes.u16(face.loca + 2u * glyph + 2);
  }
}

}  // namespace

TEST(TtfCorruptFont, DamagedGlyphsFailTheGlyphNotTheDevice) {
  // Aimed at what a render reads: the glyph's own record, its loca and hmtx entries, maxp's limits (which size
  // the scratch block) and head. The block is planned from the damaged face, at three pool sizes, and the
  // glyph and its neighbours are drawn at three sizes. The tail asserts the damage reached the loader.
  const std::vector<std::string> sources = {"Bookerly/Bookerly-Regular.ttf", "NotoSans/NotoSans-Italic.ttf",
                                            "Inter/Inter-Bold.ttf"};
  const uint32_t pools[] = {2048, kPoolBytesFlash, kPoolBytesSd};
  const uint32_t sizes[] = {ppemFromPoints(8), ppemFromPoints(14), ppemFromPoints(40)};
  Random random{20261015};
  uint32_t rounds = 0, refused = 0, damagedFailed = 0, renders = 0;
  for (const std::string& source : sources) {
    const std::vector<uint8_t> original = readFont(source);
    FontBytes intactBytes(original.data(), static_cast<uint32_t>(original.size()));
    Face intact;
    ASSERT_EQ(FaceStatus::Ok, parseFace(intactBytes, &intact));
    const size_t maxp = be32(original, findRecord(original, "maxp") + 8);
    const size_t head = be32(original, findRecord(original, "head") + 8);
    for (int round = 0; round < 1000; ++round, ++rounds) {
      std::vector<uint8_t> font = original;  // a copy, exactly the file's size
      const uint16_t glyph = static_cast<uint16_t>(1 + random.below(intact.numGlyphs - 1u));
      uint32_t start = 0, end = 0;
      glyphRecord(intactBytes, intact, glyph, &start, &end);
      const uint32_t kind = random.below(100);
      if (kind < 50 && end > start) {  // the record itself
        for (uint32_t i = 1 + random.below(8); i > 0; --i) {
          font[intact.glyf + start + random.below(end - start)] = static_cast<uint8_t>(random.next());
        }
      } else if (kind < 65 || end <= start) {  // its loca entry, or the next one (its end)
        const uint32_t entry = intact.loca + (glyph + random.below(2)) * (intact.longLoca ? 4u : 2u);
        font[entry + random.below(intact.longLoca ? 4 : 2)] = static_cast<uint8_t>(random.next());
      } else if (kind < 75) {  // its hmtx entry
        const uint32_t entry = intact.hmtx + 4u * (glyph < intact.numHMetrics ? glyph : intact.numHMetrics - 1u);
        font[entry + random.below(4)] = static_cast<uint8_t>(random.next());
      } else if (kind < 90) {  // maxp's points and contours, which size the scratch block
        set16(font, maxp + 6 + 2 * random.below(4), random.below(65));
      } else {  // head: units per em, the box, the loca format
        const uint32_t fields[] = {18, 36, 38, 40, 42, 50};
        font[head + fields[random.below(6)] + random.below(2)] = static_cast<uint8_t>(random.next());
      }
      FontBytes bytes(font.data(), static_cast<uint32_t>(font.size()));
      Face face;
      if (parseFace(bytes, &face) != FaceStatus::Ok) {
        ++refused;
        continue;
      }
      Rasterizer rasterizer;
      if (!rasterizer.allocate(planScratch(face, sizes[2], pools[round % 3]))) {
        ++refused;
        continue;
      }
      bool failed = false;
      for (const uint32_t ppem : sizes) {
        for (int neighbour = -1; neighbour <= 1; ++neighbour) {
          const int g = glyph + neighbour;
          if (g < 0 || g >= face.numGlyphs) continue;
          EpdGlyph metrics{};
          const Status status = rasterizer.render(bytes, face, static_cast<uint16_t>(g), ppem, &metrics);
          ASSERT_TRUE(status == Status::Ok || isPermanent(status)) << statusName(status);
          ++renders;
          failed |= neighbour == 0 && status != Status::Ok;
        }
      }
      damagedFailed += failed;
    }
  }
  std::printf("damaged fonts: %u rounds, %u refused at parse or plan, %u where the damaged glyph failed, %u renders\n",
              rounds, refused, damagedFailed, renders);
  EXPECT_GT(damagedFailed * 5, rounds);  // over 20% of rounds broke the glyph itself: the damage reached the loader
  EXPECT_GT(renders, rounds * 6);        // and most rounds rendered rather than being refused
}

TEST(TtfCorruptFont, HugeCoordinatesAreRefusedNotOverflowed) {
  Font font;
  font.glyphs.resize(2);
  font.glyphs[1].contours = {{{-32768, -32768}, {32767, -32768}, {32767, 32767}, {-32768, 32767}}};
  font.unitsPerEm = 16;
  font.cmap = {{'A', 1}};
  const Bytes data = buildFont(font);
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  EpdGlyph metrics{};
  EXPECT_EQ(Status::Unsupported, sharedRasterizer().render(bytes, face, 1, kMaxPpem26_6, &metrics));
}
