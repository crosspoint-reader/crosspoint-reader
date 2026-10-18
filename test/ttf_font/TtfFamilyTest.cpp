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

TEST(TtfFamily, AmongEqualWeightsTheFirstFileServes) {
  // Sorted by name: Bold, BoldCondensed (also bold), Italic, Regular.
  const ttf::FileProbe files[] = {{1, 700}, {1, 700}, {2, 400}, {0, 400}};
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(files, 4, chosen));
  EXPECT_EQ(3, chosen[0]);
  EXPECT_EQ(0, chosen[1]);  // not the condensed one that sorts after it
  EXPECT_EQ(2, chosen[2]);
  EXPECT_EQ(-1, chosen[3]);  // no bold italic: it falls back
}

TEST(TtfFamily, TheWeightClosestToRegularOrBoldServesEachStyle) {
  // A folder of static weights, sorted by name. Only Bold and BoldItalic set macStyle's bold bit, as Google
  // Fonts' static files do, so every other weight reads as regular or italic.
  const ttf::FileProbe files[] = {
      {3, 900},  // BlackItalic, which marks itself bold
      {1, 700},  // Bold
      {3, 700},  // BoldItalic
      {0, 800},  // ExtraBold
      {2, 300},  // LightItalic
      {0, 300},  // Light
      {2, 400},  // Italic
      {0, 400},  // Regular
  };
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(files, 8, chosen));
  EXPECT_EQ(7, chosen[0]);  // Regular, not the ExtraBold that sorts first
  EXPECT_EQ(1, chosen[1]);
  EXPECT_EQ(6, chosen[2]);  // Italic, not LightItalic
  EXPECT_EQ(2, chosen[3]);  // BoldItalic at 700, not 900
}

TEST(TtfFamily, WithoutAnExactWeightTheNearestServes) {
  const ttf::FileProbe files[] = {{0, 800}, {0, 300}};  // ExtraBold, Light
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(files, 2, chosen));
  EXPECT_EQ(1, chosen[0]);  // 300 is nearer 400 than 800 is
}

TEST(TtfFamily, AStaticFileWinsOverAVariableOne) {
  const ttf::FileProbe files[] = {{0, 400, true}, {0, 500}};  // the variable font sorts first
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(files, 2, chosen));
  EXPECT_EQ(1, chosen[0]);
  const ttf::FileProbe onlyVariable[] = {{0, 400, true}};
  ASSERT_TRUE(ttf::assignStyles(onlyVariable, 1, chosen));
  EXPECT_EQ(0, chosen[0]);  // drawn as its default instance (spec 5.6)
}

TEST(TtfFamily, FilesTheEngineCannotDrawAreIgnored) {
  const ttf::FileProbe files[] = {{-1}, {0}, {-1}};
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(files, 3, chosen));
  EXPECT_EQ(1, chosen[0]);
}

TEST(TtfFamily, ADirectoryWithoutARegularStyleIsNoFamily) {
  const ttf::FileProbe files[] = {{1}, {2}, {3}, {-1}};
  int chosen[4];
  EXPECT_FALSE(ttf::assignStyles(files, 4, chosen));
  EXPECT_FALSE(ttf::assignStyles(nullptr, 0, chosen));
}

TEST(TtfFamily, TheProbeReadsTheWeightClassAndSpotsVariableFonts) {
  Font font;
  font.glyphs.resize(1);
  font.glyphs[0].contours = {rect(0, 0, 64, 64)};
  font.os2 = true;
  font.weightClass = 800;
  MemoryFile extraBold{buildFont(font)};
  ttf::FileProbe probe = ttf::probeFile(extraBold.source());
  EXPECT_EQ(0, probe.style);
  EXPECT_EQ(800, probe.weight);
  EXPECT_FALSE(probe.variable);

  font.extraTables = {{"fvar", Bytes(16, 0)}};
  MemoryFile variable{buildFont(font)};
  EXPECT_TRUE(ttf::probeFile(variable.source()).variable);

  font.os2 = false;  // no OS/2 table: the weight follows the style
  font.extraTables.clear();
  font.macStyle = 1;
  MemoryFile bold{buildFont(font)};
  probe = ttf::probeFile(bold.source());
  EXPECT_EQ(1, probe.style);
  EXPECT_EQ(700, probe.weight);
}

TEST(TtfFamily, ABoldWeightDeclaringRegularServesBold) {
  // Inter's static Bold marks itself bold only by its OS/2 weight class: macStyle 0, fsSelection REGULAR.
  MemoryFile interRegular{readFont("Inter/Inter-Regular.ttf")};
  MemoryFile interBold{readFont("Inter/Inter-Bold.ttf")};
  const ttf::FileProbe inter[] = {ttf::probeFile(interBold.source()), ttf::probeFile(interRegular.source())};
  ASSERT_EQ(0, inter[0].style);
  ASSERT_EQ(700, inter[0].weight);
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(inter, 2, chosen));
  EXPECT_EQ(1, chosen[0]);  // Regular, though Bold sorts first
  EXPECT_EQ(0, chosen[1]);

  const ttf::FileProbe italics[] = {{0, 400}, {2, 400}, {2, 700}};  // the same for an italic
  ASSERT_TRUE(ttf::assignStyles(italics, 3, chosen));
  EXPECT_EQ(1, chosen[2]);
  EXPECT_EQ(2, chosen[3]);
  EXPECT_EQ(-1, chosen[1]);
}

TEST(TtfFamily, AFileDeclaringBoldWinsOverOneOfTheSameWeightThatDoesNot) {
  const ttf::FileProbe files[] = {{0, 700}, {1, 700}, {0, 400}};
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(files, 3, chosen));
  EXPECT_EQ(2, chosen[0]);
  EXPECT_EQ(1, chosen[1]);
}

TEST(TtfFamily, ASingleHeavyFaceIsARegularWithoutBold) {
  const ttf::FileProbe display[] = {{0, 900}};  // a display family with one Black face
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(display, 1, chosen));
  EXPECT_EQ(0, chosen[0]);
  EXPECT_EQ(-1, chosen[1]);  // falls back to the regular, not the same file twice in the store

  const ttf::FileProbe semiAndBlack[] = {{0, 650}, {0, 900}};  // the heavier one is still free to be the bold
  ASSERT_TRUE(ttf::assignStyles(semiAndBlack, 2, chosen));
  EXPECT_EQ(0, chosen[0]);
  EXPECT_EQ(1, chosen[1]);
}

TEST(TtfFamily, TheBundledFilesCarryTheirWeights) {
  MemoryFile regular{readFont("NotoSans/NotoSans-Regular.ttf")};
  MemoryFile bold{readFont("NotoSans/NotoSans-Bold.ttf")};
  EXPECT_EQ(400, ttf::probeFile(regular.source()).weight);
  EXPECT_EQ(700, ttf::probeFile(bold.source()).weight);
}

TEST(TtfFamily, AFamilyFitsFlashModeOnlyWithinThePartitionAndTheMappingBudget) {
  constexpr uint32_t kX3FontArea = 3538944 - 65536;
  const uint32_t bookerly[4] = {450048, 452000, 460000, 455000};
  EXPECT_TRUE(ttf::fitsFlashMode(bookerly, 4, kX3FontArea, 32));
  const uint32_t cjk[1] = {10u * 1024 * 1024};  // one CJK style is larger than the partition
  EXPECT_FALSE(ttf::fitsFlashMode(cjk, 1, kX3FontArea, 32));
  const uint32_t wide[4] = {1000000, 1000000, 1000000, 1000000};  // 4 MB of styles
  EXPECT_FALSE(ttf::fitsFlashMode(wide, 4, kX3FontArea, 32));
  EXPECT_FALSE(ttf::fitsFlashMode(bookerly, 4, kX3FontArea, 12));  // too few mapping pages for a style and a log
  EXPECT_FALSE(ttf::fitsFlashMode(bookerly, 0, kX3FontArea, 32));
}

// --- Native size font IDs ---

TEST(TtfNativeIds, AWindowHoldsItsSizesAndNothingElse) {
  const int32_t base = ttf::nativeBaseFrom(0xDEADBEEFu);
  EXPECT_EQ(0x5EADBE00, base);  // bits 8-30: positive, a multiple of 256
  EXPECT_TRUE(ttf::isNativeId(base, base | 47));
  EXPECT_EQ(47, ttf::nativeIdPixels(base | 47));
  EXPECT_FALSE(ttf::isNativeId(base, (base + 0x100) | 47));
  EXPECT_FALSE(ttf::isNativeId(base, static_cast<int>(0xDEADBE2Fu)));  // the same bits with the sign set
  EXPECT_FALSE(ttf::isNativeId(0, 47));                                // no window
}

TEST(TtfNativeIds, AHashWithoutWindowBitsGivesNoWindow) { EXPECT_EQ(0, ttf::nativeBaseFrom(0x800000FFu)); }
