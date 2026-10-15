// What the registry learns from a TTF file before loading it (its style), and the family identity.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "TtfBuilder.h"
#include "TtfFamily.h"
#include "TtfFontTestUtil.h"
#include "TtfTestUtil.h"

using namespace ttf_test;

namespace {

int probe(const Bytes& bytes) {
  MemoryFile file{bytes};
  return ttf::probeStyle(file.source());
}

Bytes builtFont(const uint16_t macStyle = 0) {
  Font font;
  font.macStyle = macStyle;
  font.glyphs.resize(1);
  font.glyphs[0].contours = {rect(0, 0, 64, 64)};
  return buildFont(font);
}

std::array<uint8_t, glyphstore::kIdentityBytes> identityOf(const ttf::FileStamp (&styles)[4]) {
  std::array<uint8_t, glyphstore::kIdentityBytes> out{};
  ttf::familyIdentity(styles, out.data());
  return out;
}

}  // namespace

TEST(TtfFamily, TheBundledBookerlyFilesDeclareTheirStyles) {
  EXPECT_EQ(0, probe(readFont("Bookerly/Bookerly-Regular.ttf")));
  EXPECT_EQ(1, probe(readFont("Bookerly/Bookerly-Bold.ttf")));
  EXPECT_EQ(2, probe(readFont("Bookerly/Bookerly-Italic.ttf")));
  EXPECT_EQ(3, probe(readFont("Bookerly/Bookerly-BoldItalic.ttf")));
}

TEST(TtfFamily, TheStyleIsTheLowTwoBitsOfMacStyle) {
  EXPECT_EQ(0, probe(builtFont(0)));
  EXPECT_EQ(1, probe(builtFont(1)));
  EXPECT_EQ(2, probe(builtFont(2)));
  EXPECT_EQ(3, probe(builtFont(3 | 0x40)));  // other macStyle bits (condensed, ...) do not matter
}

TEST(TtfFamily, CffFontsAndCollectionsAreNotTtfFamilies) {
  EXPECT_EQ(-1, probe(readFont("OpenDyslexic/OpenDyslexic-Regular.otf")));
  Bytes collection = builtFont();
  set32(collection, 0, 0x74746366);  // 'ttcf'
  EXPECT_EQ(-1, probe(collection));
}

TEST(TtfFamily, AFontMissingATableTheEngineNeedsIsRefused) {
  for (const char* table : {"cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp"}) {
    Bytes font = builtFont();
    const size_t record = findRecord(font, table);
    ASSERT_NE(0u, record) << table;
    font[record] = 'X';  // the table is no longer in the directory
    EXPECT_EQ(-1, probe(font)) << table;
  }
}

TEST(TtfFamily, ATableOutsideTheFileIsRefused) {
  Bytes font = builtFont();
  const size_t record = findRecord(font, "glyf");
  set32(font, record + 12, static_cast<uint32_t>(font.size()));  // its length runs past the end
  EXPECT_EQ(-1, probe(font));
}

TEST(TtfFamily, ATruncatedFileIsRefused) {
  const Bytes font = builtFont();
  for (const size_t keep : {size_t{0}, size_t{11}, size_t{20}, font.size() / 2, font.size() - 1}) {
    EXPECT_EQ(-1, probe(Bytes(font.begin(), font.begin() + static_cast<std::ptrdiff_t>(keep)))) << keep;
  }
}

TEST(TtfFamily, AHeadWithoutItsMagicIsRefused) {
  Bytes font = builtFont();
  const size_t record = findRecord(font, "head");
  const uint32_t head = static_cast<uint32_t>(font[record + 8] << 24 | font[record + 9] << 16 | font[record + 10] << 8 |
                                              font[record + 11]);
  set32(font, head + 12, 0x12345678);
  EXPECT_EQ(-1, probe(font));
}

TEST(TtfFamily, AReadErrorIsRefused) {
  MemoryFile file{readFont("Bookerly/Bookerly-Bold.ttf")};
  file.failReads = true;
  EXPECT_EQ(-1, ttf::probeStyle(file.source()));
}

TEST(TtfFamily, TheProbeReadsOnlyTheDirectoryAndHead) {
  MemoryFile file{readFont("NotoSans/NotoSans-Regular.ttf")};
  ASSERT_EQ(0, ttf::probeStyle(file.source()));
  EXPECT_LE(file.reads, 6u);  // the header, the directory 8 records at a time, head: cheap at discovery
}

TEST(TtfFamily, AFileSourceReadsThroughSeekAndRead) {
  struct SeekReadFile {  // HalFile's two calls
    std::vector<uint8_t> bytes;
    size_t pos = 0;
    bool seekSet(const size_t to) {
      if (to > bytes.size()) return false;
      pos = to;
      return true;
    }
    int read(void* dst, size_t n) {
      n = std::min(n, bytes.size() - pos);
      std::memcpy(dst, bytes.data() + pos, n);
      pos += n;
      return static_cast<int>(n);
    }
  };
  SeekReadFile file{readFont("Bookerly/Bookerly-Italic.ttf")};
  EXPECT_EQ(2, ttf::probeStyle(ttf::fileSource(file, static_cast<uint32_t>(file.bytes.size()))));
  const auto size = static_cast<uint32_t>(file.bytes.size());
  file.bytes.resize(100);  // the file shrank under the source: short reads fail
  EXPECT_EQ(-1, ttf::probeStyle(ttf::fileSource(file, size)));
}

TEST(TtfFamily, TheIdentityChangesWithAnyFileDetail) {
  const ttf::FileStamp base[4] = {{"Lit-Regular.ttf", 450048, 0x5A21, 0x6400},
                                  {"Lit-Bold.ttf", 452000, 0x5A21, 0x6400}};
  const auto identity = identityOf(base);
  EXPECT_EQ(identity, identityOf(base));

  ttf::FileStamp renamed[4] = {base[0], base[1]};
  renamed[1].name = "Lit-Heavy.ttf";
  ttf::FileStamp resized[4] = {base[0], base[1]};
  resized[0].size += 1;
  ttf::FileStamp redated[4] = {base[0], base[1]};
  redated[0].date += 1;
  ttf::FileStamp retimed[4] = {base[0], base[1]};
  retimed[1].time += 2;
  ttf::FileStamp moved[4] = {base[0], {}, base[1]};  // the second file now serves italic, not bold
  ttf::FileStamp fewer[4] = {base[0]};
  for (const auto* other : {&renamed, &resized, &redated, &retimed, &moved, &fewer}) {
    EXPECT_NE(identity, identityOf(*other));
  }
}

TEST(TtfFamily, TheIdentityIsSixteenIndependentBytes) {
  const ttf::FileStamp styles[4] = {{"A.ttf", 1, 2, 3}};
  const auto identity = identityOf(styles);
  // Four lanes with different seeds: no lane repeats another.
  for (int a = 0; a < 4; ++a) {
    for (int b = a + 1; b < 4; ++b) {
      EXPECT_NE(0, std::memcmp(identity.data() + 4 * a, identity.data() + 4 * b, 4)) << a << " " << b;
    }
  }
  EXPECT_EQ(glyphstore::fnv1a(identity.data(), identity.size()), ttf::identityHash(identity.data()));
}

TEST(TtfFamily, TtfFileNamesAreRecognisedInAnyCase) {
  EXPECT_TRUE(ttf::isTtfFileName("Literata-Regular.ttf"));
  EXPECT_TRUE(ttf::isTtfFileName("LITERATA.TTF"));
  EXPECT_TRUE(ttf::isTtfFileName("x.TtF"));
  EXPECT_FALSE(ttf::isTtfFileName(".ttf"));              // no name
  EXPECT_FALSE(ttf::isTtfFileName("Literata.ttf.tmp"));  // an upload in progress
  EXPECT_FALSE(ttf::isTtfFileName("Literata.otf"));      // CFF: not drawn
  EXPECT_FALSE(ttf::isTtfFileName("Literata_14.cpfont"));
  EXPECT_FALSE(ttf::isTtfFileName("Literatattf"));
}

TEST(TtfFamily, TheFirstFileOfEachStyleServesIt) {
  // Sorted by name: Bold, BoldCondensed (also bold), Italic, Regular.
  const int styles[] = {1, 1, 2, 0};
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(styles, 4, chosen));
  EXPECT_EQ(3, chosen[0]);
  EXPECT_EQ(0, chosen[1]);  // not the condensed one that sorts after it
  EXPECT_EQ(2, chosen[2]);
  EXPECT_EQ(-1, chosen[3]);  // no bold italic: it falls back
}

TEST(TtfFamily, FilesTheEngineCannotDrawAreIgnored) {
  const int styles[] = {-1, 0, -1};
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(styles, 3, chosen));
  EXPECT_EQ(1, chosen[0]);
}

TEST(TtfFamily, ADirectoryWithoutARegularStyleIsNoFamily) {
  const int styles[] = {1, 2, 3, -1};
  int chosen[4];
  EXPECT_FALSE(ttf::assignStyles(styles, 4, chosen));
  EXPECT_FALSE(ttf::assignStyles(nullptr, 0, chosen));
}
