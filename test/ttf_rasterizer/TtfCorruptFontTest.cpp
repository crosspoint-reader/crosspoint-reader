// Broken fonts (spec 9.3): cut short anywhere, or with random bytes in any table, a font never crashes the
// engine or makes it read outside the file or its scratch block. CI runs this under AddressSanitizer and
// UBSan (see CMakeLists.txt); elsewhere UBSan traps.
#include <gtest/gtest.h>

#include <cstdint>
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
