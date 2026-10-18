// What the registry learns from a TTF file before loading it (its style), and the family identity.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "TtfBuilder.h"
#include "TtfFace.h"
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

TEST(TtfFamily, ASetsIdentityChangesWithItsFamiliesAndTheirOrder) {
  const uint8_t families[3][glyphstore::kIdentityBytes] = {{'b', 'o', 'd', 'y'}, {'a', 'c', 'c'}, {'o', 't', 'h'}};
  const uint8_t swapped[2][glyphstore::kIdentityBytes] = {{'a', 'c', 'c'}, {'b', 'o', 'd', 'y'}};
  const auto identityOfSet = [](const uint8_t (*set)[glyphstore::kIdentityBytes], const uint8_t count) {
    std::array<uint8_t, glyphstore::kIdentityBytes> out{};
    ttf::setIdentity(set, count, out.data());
    return out;
  };
  const auto pair = identityOfSet(families, 2);
  EXPECT_EQ(pair, identityOfSet(families, 2));
  EXPECT_NE(pair, identityOfSet(families, 1));  // the body alone
  EXPECT_NE(pair, identityOfSet(families, 3));  // another accent family beside it
  EXPECT_NE(pair, identityOfSet(swapped, 2));   // a family's index is part of every key it stored
  EXPECT_NE(identityOfSet(families, 1), identityOfSet(families + 1, 1));
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

TEST(TtfFamily, WithoutARegularFileTheNearestStyleServesAsRegular) {
  // A script embedded as its italic alone, a display face as its bold alone: drawn in that face, held once.
  const ttf::FileProbe italicOnly[] = {{2}};
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(italicOnly, 1, chosen));
  EXPECT_EQ(0, chosen[0]);
  EXPECT_EQ(-1, chosen[2]);  // falls back to the regular, the same face

  const ttf::FileProbe boldOnly[] = {{1, 700}};
  ASSERT_TRUE(ttf::assignStyles(boldOnly, 1, chosen));
  EXPECT_EQ(0, chosen[0]);
  EXPECT_EQ(-1, chosen[1]);

  const ttf::FileProbe noRegular[] = {{1}, {2}, {3}, {-1}};  // italic first, then bold, then bold italic
  ASSERT_TRUE(ttf::assignStyles(noRegular, 4, chosen));
  EXPECT_EQ(1, chosen[0]);
  EXPECT_EQ(0, chosen[1]);
  EXPECT_EQ(-1, chosen[2]);
  EXPECT_EQ(2, chosen[3]);
}

TEST(TtfFamily, ADirectoryWithNothingTheEngineDrawsIsNoFamily) {
  const ttf::FileProbe files[] = {{-1}, {-1}};
  int chosen[4];
  EXPECT_FALSE(ttf::assignStyles(files, 2, chosen));
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

TEST(TtfFamily, AFamilyFitsTheStoreInFlashModeElseInSdMode) {
  constexpr uint32_t kX3FontArea = 3538944 - 65536;
  const uint32_t bookerly[4] = {450048, 452000, 460000, 455000};
  const uint32_t bookerlyTables[4] = {176356, 178372, 170664, 184176};
  EXPECT_TRUE(ttf::fitsStore(bookerly, bookerlyTables, 4, kX3FontArea, 32));
  // One CJK style is larger than the partition; its tables are not (DroidSansFallbackFull: 4 MB, 357 KB of tables).
  const uint32_t cjk[1] = {10u * 1024 * 1024};
  const uint32_t cjkTables[1] = {356752};
  EXPECT_TRUE(ttf::fitsStore(cjk, cjkTables, 1, kX3FontArea, 32));
  const uint32_t noTables[1] = {0};  // a file whose SD-mode block cannot be made
  EXPECT_FALSE(ttf::fitsStore(cjk, noTables, 1, kX3FontArea, 32));
  // Four styles of 1 MB with 450 KB of tables each: too large whole, and their tables, mapped together, pass the
  // mapping budget.
  const uint32_t wide[4] = {1000000, 1000000, 1000000, 1000000};
  const uint32_t wideTables[4] = {450000, 450000, 450000, 450000};
  EXPECT_FALSE(ttf::fitsStore(wide, wideTables, 4, kX3FontArea, 32));
  EXPECT_FALSE(ttf::fitsStore(bookerly, bookerlyTables, 4, kX3FontArea, 12));  // too few mapping pages either way
  EXPECT_FALSE(ttf::fitsStore(bookerly, bookerlyTables, 0, kX3FontArea, 32));
}

TEST(TtfFamily, TheProbeSaysWhatTheSdModeBlockTakes) {
  for (const char* name : {"Bookerly/Bookerly-Regular.ttf", "Inter/Inter-Bold.ttf", "NotoSans/NotoSans-Italic.ttf",
                           "Ubuntu/Ubuntu-Regular.ttf"}) {
    MemoryFile file{readFont(name)};
    ttf::SdBlock block;
    ASSERT_TRUE(block.plan(file.source())) << name;
    EXPECT_EQ(block.bytes(), ttf::probeFile(file.source()).sdBytes) << name;
  }
}

// --- SD mode's style block ---

namespace {

// The whole block, read through its source.
std::vector<uint8_t> blockImage(ttf::SdBlock& block) {
  std::vector<uint8_t> image(block.bytes());
  const ttf::FileSource source = block.source();
  EXPECT_TRUE(source.read(source.ctx, 0, image.data(), block.bytes()));
  return image;
}

}  // namespace

TEST(TtfSdBlock, IsTheFontButForItsOutlines) {
  MemoryFile file{readFont("Bookerly/Bookerly-Regular.ttf")};
  ttf::SdBlock block;
  ASSERT_TRUE(block.plan(file.source()));
  // Its ten tables the engine reads, 4-byte aligned behind an 11-record directory (fontTools agrees); glyf is
  // 208,484 bytes at 58,840 in the file.
  EXPECT_EQ(176356u, block.bytes());
  const glyphstore::SdOutlines outlines = block.outlines();
  EXPECT_EQ(block.bytes(), outlines.fontOffset);
  EXPECT_EQ(58840u, outlines.fileOffset);
  EXPECT_EQ(208484u, outlines.length);

  const std::vector<uint8_t> image = blockImage(block);
  ttf::SdPageCache cache;
  const ttf::SdRange sd{file.source(), outlines.fontOffset, outlines.fileOffset, outlines.length, &cache};
  FontBytes card(image.data(), static_cast<uint32_t>(image.size()), &sd);
  FontBytes whole(file.bytes.data(), static_cast<uint32_t>(file.bytes.size()));
  ttf::Face face;
  ttf::Face wholeFace;
  ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(card, &face));
  ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(whole, &wholeFace));
  EXPECT_EQ(wholeFace.unitsPerEm, face.unitsPerEm);
  EXPECT_EQ(wholeFace.numGlyphs, face.numGlyphs);
  EXPECT_EQ(wholeFace.numHMetrics, face.numHMetrics);
  EXPECT_EQ(wholeFace.maxPoints, face.maxPoints);
  EXPECT_EQ(wholeFace.ascender, face.ascender);
  EXPECT_EQ(wholeFace.descender, face.descender);
  EXPECT_EQ(wholeFace.height, face.height);
  EXPECT_EQ(wholeFace.cmapFormat, face.cmapFormat);
  EXPECT_EQ(wholeFace.glyfLength, face.glyfLength);
  EXPECT_EQ(outlines.fontOffset, face.glyf);
  EXPECT_NE(0u, face.gpos);
  EXPECT_NE(0u, face.gsub);
  for (const uint32_t cp : {uint32_t{'A'}, uint32_t{'z'}, uint32_t{0xE9}, uint32_t{0x2014}, uint32_t{0x4E2D}}) {
    EXPECT_EQ(ttf::glyphIndex(whole, wholeFace, cp), ttf::glyphIndex(card, face, cp)) << cp;
  }
  // Every table but glyf is in place, byte for byte the file's; glyf never is.
  ASSERT_NE(nullptr, card.span(face.gpos, face.gposLength));
  EXPECT_EQ(0, std::memcmp(whole.span(wholeFace.gpos, wholeFace.gposLength), card.span(face.gpos, face.gposLength),
                           face.gposLength));
  EXPECT_EQ(0, std::memcmp(whole.span(wholeFace.loca, wholeFace.locaLength), card.span(face.loca, face.locaLength),
                           face.locaLength));
  EXPECT_EQ(nullptr, card.span(face.glyf, 1));
  EXPECT_FALSE(card.outOfRange());
  EXPECT_EQ(0u, cache.misses());  // nothing of the outlines read yet
}

TEST(TtfSdBlock, AnyRangeReadsAsTheWholeBlockDoes) {
  MemoryFile file{readFont("Inter/Inter-Regular.ttf")};
  ttf::SdBlock block;
  ASSERT_TRUE(block.plan(file.source()));
  const std::vector<uint8_t> image = blockImage(block);
  const ttf::FileSource source = block.source();
  // Pieces of every size: the directory a byte at a time, across a table's end and its padding, a copy chunk.
  for (const uint32_t piece : {1u, 7u, 333u, 4096u}) {
    std::vector<uint8_t> pieces(image.size());
    for (uint32_t at = 0; at < image.size(); at += piece) {
      const uint32_t len = std::min<uint32_t>(piece, static_cast<uint32_t>(image.size()) - at);
      ASSERT_TRUE(source.read(source.ctx, at, pieces.data() + at, len)) << piece << " at " << at;
    }
    EXPECT_EQ(image, pieces) << piece;
  }
  uint8_t past = 0;
  EXPECT_FALSE(source.read(source.ctx, block.bytes(), &past, 1));
}

TEST(TtfSdBlock, AnOptionalTableOutsideTheFileIsLeftOutAsTheFaceLeavesItOut) {
  // A GPOS record past the end of the file: parseFace() takes the font as having no GPOS, so the block copies none,
  // and the probe's size says so too.
  Font font;
  font.glyphs.resize(1);
  font.glyphs[0].contours = {rect(0, 0, 64, 64)};
  font.extraTables = {{"GPOS", Bytes(64, 0)}};
  Bytes bytes = buildFont(font);
  set32(bytes, findRecord(bytes, "GPOS") + 12, static_cast<uint32_t>(bytes.size()));
  MemoryFile file{bytes};
  ttf::SdBlock block;
  ASSERT_TRUE(block.plan(file.source()));
  EXPECT_EQ(block.bytes(), ttf::probeFile(file.source()).sdBytes);
  const std::vector<uint8_t> image = blockImage(block);
  const glyphstore::SdOutlines outlines = block.outlines();
  ttf::SdPageCache cache;
  const ttf::SdRange sd{file.source(), outlines.fontOffset, outlines.fileOffset, outlines.length, &cache};
  FontBytes card(image.data(), static_cast<uint32_t>(image.size()), &sd);
  ttf::Face face;
  ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(card, &face));
  EXPECT_EQ(0u, face.gpos);
  EXPECT_TRUE(block.sameAs(image.data(), static_cast<uint32_t>(image.size())));
}

TEST(TtfSdBlock, AFileWithoutOutlinesOrNotTrueTypeHasNone) {
  ttf::SdBlock block;
  Bytes noGlyf = builtFont();
  noGlyf[findRecord(noGlyf, "glyf") + 3] = 'X';  // 'glyX'
  MemoryFile withoutOutlines{noGlyf};
  EXPECT_FALSE(block.plan(withoutOutlines.source()));
  Bytes cff = builtFont();
  std::memcpy(cff.data(), "OTTO", 4);
  MemoryFile notTrueType{cff};
  EXPECT_FALSE(block.plan(notTrueType.source()));
  Bytes broken = builtFont();
  set32(broken, findRecord(broken, "hmtx") + 12, static_cast<uint32_t>(broken.size()));  // runs past the end
  MemoryFile outside{broken};
  EXPECT_FALSE(block.plan(outside.source()));
  MemoryFile fine{builtFont()};
  EXPECT_TRUE(block.plan(fine.source()));
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
