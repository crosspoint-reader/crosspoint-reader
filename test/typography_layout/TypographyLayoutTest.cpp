#include <Epub/ParsedText.h>
#include <Epub/ReaderRenderSpec.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <SdCardFont.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {
size_t gFailNothrowArrayAllocSize = 0;
size_t gDeniedNothrowArrayAllocs = 0;
}  // namespace

void* operator new[](const size_t size) {
  void* allocation = std::malloc(size == 0 ? 1 : size);
  if (!allocation) {
    std::fputs("Unexpected host OOM in TypographyLayoutTest\n", stderr);
    std::exit(EXIT_FAILURE);
  }
  return allocation;
}

void* operator new[](const size_t size, const std::nothrow_t&) noexcept {
  if (gFailNothrowArrayAllocSize != 0 && size == gFailNothrowArrayAllocSize) {
    ++gDeniedNothrowArrayAllocs;
    return nullptr;
  }
  return std::malloc(size == 0 ? 1 : size);
}

void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation, size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, const std::nothrow_t&) noexcept { std::free(allocation); }

namespace {

void putU16(std::vector<uint8_t>& buf, const size_t off, const uint16_t val) {
  buf[off] = static_cast<uint8_t>(val & 0xFF);
  buf[off + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

void putI16(std::vector<uint8_t>& buf, const size_t off, const int16_t val) {
  putU16(buf, off, static_cast<uint16_t>(val));
}

void putU32(std::vector<uint8_t>& buf, const size_t off, const uint32_t val) {
  putU16(buf, off, static_cast<uint16_t>(val & 0xFFFF));
  putU16(buf, off + 2, static_cast<uint16_t>((val >> 16) & 0xFFFF));
}

struct GlyphSpec {
  uint32_t cp;
  uint16_t advanceFP;
};

// Builds an in-memory v4 1-bit .cpfont with Bookerly 14pt fp4 metrics plus
// Hebrew, CJK, and Hangul glyphs for host layout verification against unmodified
// SdCardFont / EpdFont / GfxRenderer.
void buildBookerly14CpFont(const bool includeBold = false) {
  static constexpr GlyphSpec kBaseSpecs[] = {
      {' ', 100},    {'!', 140},    {'"', 148},    {'\'', 88},    {'(', 150},    {')', 150},    {',', 125},
      {'-', 174},    {'.', 125},    {'0', 280},    {'1', 280},    {'2', 280},    {'3', 280},    {'4', 280},
      {'5', 280},    {'6', 280},    {'7', 280},    {'8', 280},    {'9', 280},    {':', 125},    {';', 125},
      {'?', 210},    {'A', 344},    {'B', 287},    {'C', 298},    {'D', 348},    {'E', 292},    {'F', 264},
      {'G', 337},    {'H', 387},    {'I', 174},    {'J', 165},    {'K', 333},    {'L', 286},    {'M', 415},
      {'N', 379},    {'O', 360},    {'P', 274},    {'Q', 360},    {'R', 309},    {'S', 255},    {'T', 316},
      {'U', 359},    {'V', 341},    {'W', 515},    {'X', 337},    {'Y', 316},    {'Z', 274},    {'a', 254},
      {'b', 280},    {'c', 235},    {'d', 289},    {'e', 240},    {'f', 187},    {'g', 268},    {'h', 307},
      {'i', 151},    {'j', 129},    {'k', 275},    {'l', 148},    {'m', 462},    {'n', 309},    {'o', 270},
      {'p', 287},    {'q', 276},    {'r', 210},    {'s', 221},    {'t', 189},    {'u', 304},    {'v', 280},
      {'w', 415},    {'x', 273},    {'y', 280},    {'z', 230},    {0x00AB, 200}, {0x00BB, 200}, {0x2010, 174},
      {0x2011, 174}, {0x2012, 250}, {0x2013, 250}, {0x2014, 448}, {0x2018, 96},  {0x2019, 96},  {0x201C, 128},
      {0x201D, 128}, {0x2026, 320}, {0x2039, 128}, {0x203A, 128}, {0x3002, 288}, {0x300C, 288}, {0x300D, 288},
      {0x4E2D, 288}, {0x6587, 288}, {0xAC00, 288}, {0xAD6D, 288}, {0xADF8, 288}, {0xB098, 288}, {0xB2E4, 288},
      {0xB300, 288}, {0xB77C, 288}, {0xB798, 288}, {0xB85C, 288}, {0xB9C8, 288}, {0xBBFC, 288}, {0xBC0D, 288},
      {0xBC14, 288}, {0xC0AC, 288}, {0xCEF4, 288}, {0xD130, 288}, {0xD4E8, 288}, {0xD504, 288}, {0xD55C, 288},
      {0xFB00, 348}, {0xFB01, 313}, {0xFB02, 309}, {0xFB03, 475}, {0xFB04, 471}, {0xFF0C, 288}, {0xFFFD, 467},
  };

  std::vector<GlyphSpec> sorted(std::begin(kBaseSpecs), std::end(kBaseSpecs));
  // Add all 27 Hebrew letters (U+05D0..U+05EA) for RTL and BiDi justification tests.
  for (uint32_t cp = 0x05D0; cp <= 0x05EA; ++cp) {
    sorted.push_back({cp, 256});
  }
  std::sort(sorted.begin(), sorted.end(), [](const GlyphSpec& a, const GlyphSpec& b) { return a.cp < b.cp; });

  std::vector<EpdUnicodeInterval> intervals;
  uint32_t startIdx = 0;
  for (size_t i = 1; i <= sorted.size(); ++i) {
    if (i == sorted.size() || sorted[i].cp != sorted[i - 1].cp + 1) {
      intervals.push_back({sorted[startIdx].cp, sorted[i - 1].cp, startIdx});
      startIdx = static_cast<uint32_t>(i);
    }
  }

  constexpr size_t HEADER_BYTES = 32;
  constexpr size_t TOC_BYTES = 32;
  const uint8_t styleCount = includeBold ? 2 : 1;
  const size_t firstDataOffset = HEADER_BYTES + styleCount * TOC_BYTES;
  const size_t intervalsBytes = intervals.size() * sizeof(EpdUnicodeInterval);
  const size_t glyphsBytes = sorted.size() * sizeof(EpdGlyph);
  constexpr uint16_t perGlyphBmpBytes = 1;  // 1x8 1bpp marker bitmap
  const size_t bitmapBytes = sorted.size() * perGlyphBmpBytes;
  const size_t perStyleBytes = intervalsBytes + glyphsBytes + bitmapBytes;

  sdFontTestFile.assign(firstDataOffset + styleCount * perStyleBytes, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  putU16(sdFontTestFile, 8, CPFONT_VERSION);
  putU16(sdFontTestFile, 10, 0);
  sdFontTestFile[12] = styleCount;

  for (uint8_t si = 0; si < styleCount; ++si) {
    const size_t tocOff = HEADER_BYTES + si * TOC_BYTES;
    const size_t dataOffset = firstDataOffset + si * perStyleBytes;
    sdFontTestFile[tocOff + 0] = si;  // styleId = REGULAR(0), BOLD(1)
    putU32(sdFontTestFile, tocOff + 4, static_cast<uint32_t>(intervals.size()));
    putU32(sdFontTestFile, tocOff + 8, static_cast<uint32_t>(sorted.size()));
    sdFontTestFile[tocOff + 12] = 34;  // advanceY
    putI16(sdFontTestFile, tocOff + 13, 27);
    putI16(sdFontTestFile, tocOff + 15, -8);
    putU32(sdFontTestFile, tocOff + 24, static_cast<uint32_t>(dataOffset));

    size_t cur = dataOffset;
    std::memcpy(sdFontTestFile.data() + cur, intervals.data(), intervalsBytes);
    cur += intervalsBytes;

    uint32_t bmOff = 0;
    for (size_t i = 0; i < sorted.size(); ++i) {
      EpdGlyph g{};
      if (sorted[i].cp != ' ') {
        g.width = 1;
        g.height = 8;
        g.left = 0;
        g.top = 8;
        g.dataLength = perGlyphBmpBytes;
        g.dataOffset = bmOff;
        bmOff += perGlyphBmpBytes;
      }
      g.advanceX = static_cast<uint16_t>(sorted[i].advanceFP + (si == 1 && sorted[i].cp != ' ' ? 16 : 0));
      std::memcpy(sdFontTestFile.data() + cur + i * sizeof(EpdGlyph), &g, sizeof(EpdGlyph));
    }
    cur += glyphsBytes;
    std::memset(sdFontTestFile.data() + cur, 0xFF, bmOff);
  }
}

bool isLogicalPixelBlack(const GfxRenderer& renderer, const int x, const int y) {
  const uint8_t* fb = renderer.getFrameBuffer();
  const int phyX = y;
  const int phyY = static_cast<int>(renderer.getDisplayHeight()) - 1 - x;
  if (phyX < 0 || phyX >= renderer.getDisplayWidth() || phyY < 0 || phyY >= renderer.getDisplayHeight()) {
    return false;
  }
  const size_t byteIdx = static_cast<size_t>(phyY) * renderer.getDisplayWidthBytes() + static_cast<size_t>(phyX / 8);
  const uint8_t mask = static_cast<uint8_t>(1u << (7 - (phyX % 8)));
  return (fb[byteIdx] & mask) == 0;
}

std::vector<int> scanDrawnColumnsAtRow(const GfxRenderer& renderer, const int y, const int maxX = 528) {
  std::vector<int> cols;
  for (int x = 0; x < maxX; ++x) {
    if (isLogicalPixelBlack(renderer, x, y)) {
      cols.push_back(x);
    }
  }
  return cols;
}

int expectedRightOpticalHang(const GfxRenderer& renderer, const int fontId, const std::string& word,
                             const EpdFontFamily::Style style) {
  if (word.empty()) return 0;
  const auto* scan = reinterpret_cast<const unsigned char*>(word.c_str());
  while (*scan) {
    if (utf8IsCjkBreakable(utf8NextCodepoint(&scan))) return 0;
  }
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) --i;
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.data() + i);
  const uint32_t cp = utf8NextCodepoint(&ptr);
  int pct = 0;
  switch (cp) {
    case '-':
    case ',':
    case '.':
    case '"':
    case '\'':
    case 0x00BB:
    case 0x2010:
    case 0x2011:
    case 0x2019:
    case 0x201D:
    case 0x203A:
      pct = 40;
      break;
    case 0x2012:
    case 0x2013:
    case 0x2014:
    case 0x2026:
      pct = 25;
      break;
    default:
      return 0;
  }
  char cpBuf[5] = {};
  utf8AppendCodepoint(cpBuf, cp);
  const int adv = renderer.getTextAdvanceX(fontId, cpBuf, style, 0, BidiUtils::BidiBaseDir::LTR);
  if (adv <= 0) return 0;
  return std::min(adv, std::clamp((adv * pct + 50) / 100, 1, 3));
}

int measuredBlockWordWidth(const GfxRenderer& renderer, const int fontId, const TextBlock& line, const uint16_t idx) {
  const std::string text = line.wordText(idx);
  const EpdFontFamily::Style style = line.wordStyle(idx);
  const uint8_t fb = line.focusBoundary(idx);
  if (fb == 0 || fb >= text.size()) {
    return renderer.getTextAdvanceX(fontId, text.c_str(), style);
  }
  const auto boldStyle = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
  const std::string prefix = text.substr(0, fb);
  const std::string suffix = text.substr(fb);
  return renderer.getTextAdvanceX(fontId, prefix.c_str(), boldStyle) +
         renderer.getTextAdvanceX(fontId, suffix.c_str(), style);
}

std::string reconstructParagraph(const std::vector<std::unique_ptr<TextBlock>>& lines) {
  std::string out;
  for (size_t li = 0; li < lines.size(); ++li) {
    const TextBlock& line = *lines[li];
    EXPECT_TRUE(line.valid()) << "Invalid TextBlock at line " << li;
    EXPECT_GT(line.wordCount(), 0u) << "Empty TextBlock at line " << li;
    for (uint16_t wi = 0; wi < line.wordCount(); ++wi) {
      std::string piece = line.wordText(wi);
      if (line.wordHasDiscretionaryHyphen(wi)) {
        EXPECT_EQ(wi + 1, line.wordCount()) << "Mid-line discretionary hyphen at line " << li;
        if (!piece.empty() && piece.back() == '-') {
          piece.pop_back();
        }
      }
      out += piece;
    }
  }
  return out;
}

std::vector<std::string> splitWhitespace(const char* text) {
  std::vector<std::string> out;
  std::string cur;
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p == ' ' || *p == '\n' || *p == '\t') {
      if (!cur.empty()) {
        out.push_back(std::move(cur));
        cur.clear();
      }
    } else {
      cur.push_back(*p);
    }
  }
  if (!cur.empty()) {
    out.push_back(std::move(cur));
  }
  return out;
}

class TypographyLayoutFixture : public ::testing::Test {
 protected:
  static constexpr int kFontId = 42;
  HalDisplay display;
  GfxRenderer renderer{display};
  SdCardFont sdFont;
  std::unique_ptr<FontCacheManager> cacheMgr;

  void SetUp() override {
    renderer.begin();
    renderer.clearScreen(0xFF);
    buildBookerly14CpFont();
    ASSERT_TRUE(sdFont.load("/fonts/Bookerly_14.cpfont"));
    renderer.insertFont(kFontId, EpdFontFamily(14, sdFont.getEpdFont(0)));
    renderer.registerSdCardFont(kFontId, &sdFont);
    cacheMgr =
        std::make_unique<FontCacheManager>(renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts());
    renderer.setFontCacheManager(cacheMgr.get());
    Hyphenator::setPreferredLanguage("en");
  }
};

}  // namespace

// ============================================================================
// Bresenham Justification Across LTR, RTL, and BiDi
// ============================================================================

TEST_F(TypographyLayoutFixture, BresenhamJustificationAlignsFlushRightAcrossLtrRtlAndBidi) {
  // 1. LTR: test multiple column widths so spareSpace % gapCount exercises non-zero remainders
  //    with 0 px right-margin wobble and <= 1 px inter-gap spread.
  {
    BlockStyle style;
    style.alignment = CssTextAlign::Justify;
    style.textIndentDefined = true;
    style.textIndent = 0;

    const char* words[] = {"When",     "modern",  "portable", "readers",  "render",  "classic", "serif",
                           "type",     "across",  "narrow",   "columns,", "careful", "spacing", "between",
                           "adjacent", "words",   "keeps",    "every",    "margin",  "clean",   "and",
                           "steady",   "through", "every",    "single",   "chapter", "of",      "prose."};
    for (const uint16_t pageWidth : {uint16_t{497}, uint16_t{503}, uint16_t{508}, uint16_t{513}, uint16_t{518}}) {
      ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
      for (const char* w : words) para.addWord(w, EpdFontFamily::REGULAR);

      std::vector<std::unique_ptr<TextBlock>> lines;
      para.layoutAndExtractLines(renderer, kFontId, pageWidth,
                                 [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
      ASSERT_GE(lines.size(), 3u);

      for (size_t li = 0; li + 1 < lines.size(); ++li) {
        const TextBlock& line = *lines[li];
        ASSERT_GT(line.wordCount(), 1u);
        const uint16_t lastIdx = line.wordCount() - 1;
        const int rightHang =
            expectedRightOpticalHang(renderer, kFontId, line.wordText(lastIdx), line.wordStyle(lastIdx));
        const int rightEdge =
            line.wordXpos(lastIdx) + renderer.getTextAdvanceX(kFontId, line.wordText(lastIdx), line.wordStyle(lastIdx));
        EXPECT_EQ(rightEdge, static_cast<int>(pageWidth) + rightHang) << "LTR line=" << li << " col=" << pageWidth;

        int minGap = INT32_MAX;
        int maxGap = INT32_MIN;
        for (uint16_t wi = 0; wi + 1 < line.wordCount(); ++wi) {
          const int gap =
              line.wordXpos(wi + 1) -
              (line.wordXpos(wi) + renderer.getTextAdvanceX(kFontId, line.wordText(wi), line.wordStyle(wi)));
          minGap = std::min(minGap, gap);
          maxGap = std::max(maxGap, gap);
        }
        EXPECT_LE(maxGap - minGap, 1) << "Bresenham gap spread > 1 px on LTR line=" << li;
      }
    }

    // Sparse 2-word justified line reaches exact right margin; 1-word (0-gap) line stays left-aligned.
    ParsedText sparsePara(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    sparsePara.addWord("brash,", EpdFontFamily::REGULAR);
    sparsePara.addWord("quick", EpdFontFamily::REGULAR);
    sparsePara.addWord("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> sparseLines;
    sparsePara.layoutAndExtractLines(
        renderer, kFontId, 508, [&](std::unique_ptr<TextBlock> b, uint32_t) { sparseLines.push_back(std::move(b)); });
    ASSERT_GE(sparseLines.size(), 2u);
    ASSERT_EQ(sparseLines[0]->wordCount(), 2u);
    EXPECT_EQ(sparseLines[0]->wordXpos(0), 0);
    const int w1 = renderer.getTextAdvanceX(kFontId, sparseLines[0]->wordText(1), EpdFontFamily::REGULAR);
    EXPECT_EQ(sparseLines[0]->wordXpos(1) + w1, 508);

    ParsedText singlePara(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    singlePara.addWord("brash,", EpdFontFamily::REGULAR);
    singlePara.addWord("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> singleLines;
    singlePara.layoutAndExtractLines(
        renderer, kFontId, 508, [&](std::unique_ptr<TextBlock> b, uint32_t) { singleLines.push_back(std::move(b)); });
    ASSERT_GE(singleLines.size(), 2u);
    ASSERT_EQ(singleLines[0]->wordCount(), 1u);
    EXPECT_EQ(singleLines[0]->wordXpos(0), 0);
  }

  // 2. Pure RTL (Hebrew) justified lines: 0 px wobble on both left (x == 0) and right (pageWidth) margins.
  {
    BlockStyle rtlStyle;
    rtlStyle.alignment = CssTextAlign::Justify;
    rtlStyle.isRtl = true;
    rtlStyle.directionDefined = true;
    rtlStyle.textIndentDefined = true;
    rtlStyle.textIndent = 0;

    const char* hebrew[] = {"שלום", "עולם", "ספר",   "חדש",  "קורא", "דפים", "יפה", "טוב",  "אחד",
                            "שני",  "מילה", "גדולה", "שלום", "עולם", "ספר",  "חדש", "קורא", "דפים"};
    for (const uint16_t pageWidth : {uint16_t{323}, uint16_t{331}, uint16_t{337}, uint16_t{343}}) {
      ParsedText para(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, rtlStyle, 0);
      for (const char* w : hebrew) para.addWord(w, EpdFontFamily::REGULAR);

      std::vector<std::unique_ptr<TextBlock>> lines;
      para.layoutAndExtractLines(renderer, kFontId, pageWidth,
                                 [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
      ASSERT_GE(lines.size(), 3u);

      for (size_t li = 0; li + 1 < lines.size(); ++li) {
        const TextBlock& line = *lines[li];
        ASSERT_GT(line.wordCount(), 1u);
        int minLeft = INT32_MAX;
        int maxRight = INT32_MIN;
        for (uint16_t wi = 0; wi < line.wordCount(); ++wi) {
          const int x = line.wordXpos(wi);
          const int w = renderer.getTextAdvanceX(kFontId, line.wordText(wi), line.wordStyle(wi));
          minLeft = std::min(minLeft, x);
          maxRight = std::max(maxRight, x + w);
        }
        EXPECT_EQ(minLeft, 0) << "RTL left margin wobble on line=" << li << " col=" << pageWidth;
        EXPECT_EQ(maxRight, static_cast<int>(pageWidth))
            << "RTL right margin wobble on line=" << li << " col=" << pageWidth;
      }
    }
  }

  // 3. Mixed BiDi (both LTR-base and RTL-base) exercises the visual-reordered JustifyDistributor path.
  for (const bool baseRtl : {false, true}) {
    BlockStyle bidiStyle;
    bidiStyle.alignment = CssTextAlign::Justify;
    bidiStyle.isRtl = baseRtl;
    bidiStyle.directionDefined = true;
    bidiStyle.textIndentDefined = true;
    bidiStyle.textIndent = 0;

    const char* mixed[] = {"שלום",  "alpha", "beta",  "עולם", "gamma", "ספר", "delta", "חדש",
                           "omega", "קורא",  "sigma", "דפים", "theta", "יפה", "kappa", "טוב"};
    for (const uint16_t pageWidth : {uint16_t{323}, uint16_t{331}, uint16_t{337}}) {
      ParsedText para(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, bidiStyle, 0);
      for (const char* w : mixed) para.addWord(w, EpdFontFamily::REGULAR);

      std::vector<std::unique_ptr<TextBlock>> lines;
      para.layoutAndExtractLines(renderer, kFontId, pageWidth,
                                 [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
      ASSERT_GE(lines.size(), 3u);

      for (size_t li = 0; li + 1 < lines.size(); ++li) {
        const TextBlock& line = *lines[li];
        ASSERT_GT(line.wordCount(), 1u);
        int minLeft = INT32_MAX;
        int maxRight = INT32_MIN;
        for (uint16_t wi = 0; wi < line.wordCount(); ++wi) {
          const int x = line.wordXpos(wi);
          const int w = renderer.getTextAdvanceX(kFontId, line.wordText(wi), line.wordStyle(wi));
          minLeft = std::min(minLeft, x);
          maxRight = std::max(maxRight, x + w);
        }
        EXPECT_EQ(minLeft, 0) << "BiDi left wobble baseRtl=" << baseRtl << " line=" << li;
        EXPECT_EQ(maxRight, static_cast<int>(pageWidth))
            << "BiDi right wobble baseRtl=" << baseRtl << " line=" << li << " col=" << pageWidth;
      }
    }
  }
}

// ============================================================================
// Knuth-Plass DP (Unhyphenated & Hyphenated with 2/3 English Bounds)
// ============================================================================

TEST_F(TypographyLayoutFixture, KnuthPlassBalancesUnhyphenatedAndHyphenatedParagraphs) {
  // 1. Unhyphenated KP DP globally balances slack across lines instead of leaving a
  //    single-word loose line after a tightly packed greedy first line.
  {
    BlockStyle style;
    style.alignment = CssTextAlign::Justify;
    style.textIndentDefined = true;
    style.textIndent = 0;

    renderer.ensureSdCardFontReady(kFontId, "In a dim old hall we read chapters internationalization.", 0x01);
    const char* line0Words[] = {"In", "a", "dim", "old", "hall", "we", "read"};
    int line0GreedyW = 0;
    {
      ParsedText probe(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
      for (const char* w : line0Words) probe.addWord(w, EpdFontFamily::REGULAR);
      probe.layoutAndExtractLines(renderer, kFontId, 900, [&](std::unique_ptr<TextBlock> b, uint32_t) {
        const uint16_t last = b->wordCount() - 1;
        line0GreedyW = b->wordXpos(last) + renderer.getTextAdvanceX(kFontId, b->wordText(last), b->wordStyle(last));
      });
    }
    const uint16_t colW = static_cast<uint16_t>(line0GreedyW + 2);

    ParsedText dpPara(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : line0Words) dpPara.addWord(w, EpdFontFamily::REGULAR);
    dpPara.addWord("chapters", EpdFontFamily::REGULAR);
    dpPara.addWord("internationalization.", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> dpLines;
    dpPara.layoutAndExtractLines(renderer, kFontId, colW,
                                 [&](std::unique_ptr<TextBlock> b, uint32_t) { dpLines.push_back(std::move(b)); });

    ASSERT_EQ(dpLines.size(), 3u);
    // Greedy packs all 7 words on line 0 and leaves "chapters" alone on line 1 (0 gaps).
    // Knuth-Plass pulls trailing word(s) down onto line 1 so both justified lines have balanced stretchable gaps.
    EXPECT_LT(dpLines[0]->wordCount(), 7u);
    EXPECT_GE(dpLines[1]->wordCount(), 2u);
    EXPECT_STREQ(dpLines[1]->wordText(dpLines[1]->wordCount() - 1), "chapters");
    EXPECT_EQ(reconstructParagraph(dpLines), "Inadimoldhallwereadchaptersinternationalization.");
  }

  // 2. Hyphenated KP DP with 2/3 English bounds takes 2-letter prefixes ("De-", "mo-", "de-")
  //    to eliminate white rivers while keeping 1-letter prefixes ("around") and 2-letter suffixes
  //    ("walked") unsplit.
  {
    BlockStyle style;
    style.alignment = CssTextAlign::Justify;
    style.textIndentDefined = true;
    style.textIndent = 0;

    renderer.ensureSdCardFontReady(
        kFontId, "When clear Detroit mobile devices around walked De- mo- de- observations concluded.", 0x01);
    const int baseW = renderer.getTextAdvanceX(kFontId, "When", EpdFontFamily::REGULAR) +
                      renderer.getSpaceAdvance(kFontId, 'n', 'c', EpdFontFamily::REGULAR) +
                      renderer.getTextAdvanceX(kFontId, "clear", EpdFontFamily::REGULAR) +
                      renderer.getSpaceAdvance(kFontId, 'r', 'D', EpdFontFamily::REGULAR);

    const struct TwoThreeCase {
      const char* word;
      const char* expectedPrefix;
    } kCases[] = {
        {"Detroit", "De-"},
        {"mobile", "mo-"},
        {"devices", "de-"},
    };
    for (const auto& tc : kCases) {
      SCOPED_TRACE(tc.word);
      const int preW = renderer.getTextAdvanceX(kFontId, tc.expectedPrefix, EpdFontFamily::REGULAR);
      const uint16_t colW = static_cast<uint16_t>(baseW + preW);

      ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
      para.addWord("When", EpdFontFamily::REGULAR);
      para.addWord("clear", EpdFontFamily::REGULAR);
      para.addWord(tc.word, EpdFontFamily::REGULAR);
      para.addWord("observations", EpdFontFamily::REGULAR);
      para.addWord("concluded.", EpdFontFamily::REGULAR);

      std::vector<std::unique_ptr<TextBlock>> lines;
      para.layoutAndExtractLines(renderer, kFontId, colW,
                                 [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
      ASSERT_GE(lines.size(), 2u);
      const uint16_t last0 = lines[0]->wordCount() - 1;
      EXPECT_TRUE(lines[0]->wordHasDiscretionaryHyphen(last0));
      EXPECT_STREQ(lines[0]->wordText(last0), tc.expectedPrefix);
      EXPECT_EQ(reconstructParagraph(lines), std::string("Whenclear") + tc.word + "observationsconcluded.");
    }

    // 1-letter prefix ("around") and 2-letter suffix ("walked") stay unsplit under 2/3 rules.
    for (const char* unsplit : {"around", "walked"}) {
      SCOPED_TRACE(unsplit);
      const uint16_t colW = static_cast<uint16_t>(baseW + 24);
      ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
      para.addWord("When", EpdFontFamily::REGULAR);
      para.addWord("clear", EpdFontFamily::REGULAR);
      para.addWord(unsplit, EpdFontFamily::REGULAR);
      std::vector<std::unique_ptr<TextBlock>> lines;
      para.layoutAndExtractLines(renderer, kFontId, colW,
                                 [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
      ASSERT_EQ(lines.size(), 2u);
      EXPECT_FALSE(lines[0]->wordHasDiscretionaryHyphen(lines[0]->wordCount() - 1));
      EXPECT_STREQ(lines[0]->wordText(lines[0]->wordCount() - 1), "clear");
      EXPECT_STREQ(lines[1]->wordText(0), unsplit);
    }
  }

  // 3. Consecutive-hyphen demerit avoids back-to-back discretionary hyphens when clean breaks exist,
  //    and mid-word continuation lines splitting a later word preserve flush right alignment.
  {
    BlockStyle style;
    style.alignment = CssTextAlign::Justify;
    style.textIndentDefined = true;
    style.textIndent = 0;

    ParsedText consec(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    const char* words[] = {"That",   "led",  "him",     "to",       "Tony",       "Fadell,",
                           "a",      "very", "notable", "creative", "specialist", "working",
                           "within", "many", "modern",  "devices",  "everywhere."};
    for (const char* w : words) consec.addWord(w, EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> lines;
    consec.layoutAndExtractLines(renderer, kFontId, 518,
                                 [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    size_t backToBackHyphens = 0;
    for (size_t li = 1; li < lines.size(); ++li) {
      const auto& prev = *lines[li - 1];
      const auto& curr = *lines[li];
      if (prev.wordHasDiscretionaryHyphen(prev.wordCount() - 1) &&
          curr.wordHasDiscretionaryHyphen(curr.wordCount() - 1)) {
        ++backToBackHyphens;
      }
    }
    EXPECT_EQ(backToBackHyphens, 0u);

    ParsedText splitCont(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    const char* contWords[] = {"That",    "led",    "him",    "to",          "Tony",         "Fadell,",
                               "a",       "brash,", "quick-", "minded",      "entrepreneur", "from",
                               "Detroit", "with",   "an",     "engineering", "mind",         "and",
                               "a",       "knack",  "for",    "punchy",      "storytelling."};
    for (const char* w : contWords) splitCont.addWord(w, EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> contLines;
    splitCont.layoutAndExtractLines(renderer, kFontId, 448,
                                    [&](std::unique_ptr<TextBlock> b, uint32_t) { contLines.push_back(std::move(b)); });
    ASSERT_GE(contLines.size(), 4u);
    for (size_t li = 0; li + 1 < contLines.size(); ++li) {
      const TextBlock& line = *contLines[li];
      const uint16_t lastIdx = line.wordCount() - 1;
      const int rightHang =
          expectedRightOpticalHang(renderer, kFontId, line.wordText(lastIdx), line.wordStyle(lastIdx));
      const int rightEdge =
          line.wordXpos(lastIdx) + renderer.getTextAdvanceX(kFontId, line.wordText(lastIdx), line.wordStyle(lastIdx));
      EXPECT_EQ(rightEdge, 448 + rightHang) << "Mid-word continuation line=" << li;
    }
  }
}

// ============================================================================
// Moby-Dick Ch. 1 Page 1 Prose Layout & Optical Margins
// ============================================================================

TEST_F(TypographyLayoutFixture, MobyDickChapter1Page1JustifiedLayoutAndOpticalMargins) {
  static constexpr const char* kMobyDickOpening =
      "Call me Ishmael. Some years ago\xE2\x80\x94never mind how long precisely\xE2\x80\x94having little or no "
      "money in my purse, and nothing particular to interest me on shore, I thought I would sail about a little and "
      "see the watery part of the world. It is a way I have of driving off the spleen and regulating the circulation. "
      "Whenever I find myself growing grim about the mouth; whenever it is a damp, drizzly November in my soul; "
      "whenever I find myself involuntarily pausing before coffin warehouses, and bringing up the rear of every "
      "funeral I meet; and especially whenever my hypos get such an upper hand of me, that it requires a strong moral "
      "principle to prevent me from deliberately stepping into the street, and methodically knocking people's hats "
      "off\xE2\x80\x94then, I account it high time to get to sea as soon as I can.";

  const std::vector<std::string> tokens = splitWhitespace(kMobyDickOpening);
  std::string expectedConcat;
  for (const auto& t : tokens) expectedConcat += t;

  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  bool sawHyphenProtrusion = false;
  bool sawPunctProtrusion = false;
  for (const uint16_t colW : {uint16_t{360}, uint16_t{420}, uint16_t{468}, uint16_t{518}}) {
    SCOPED_TRACE(colW);
    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    for (const auto& t : tokens) para.addWord(t, EpdFontFamily::REGULAR);

    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, colW,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 8u);
    EXPECT_EQ(reconstructParagraph(lines), expectedConcat);

    for (size_t li = 0; li + 1 < lines.size(); ++li) {
      const TextBlock& line = *lines[li];
      ASSERT_GT(line.wordCount(), 1u);
      const uint16_t lastIdx = line.wordCount() - 1;
      const std::string lastWord = line.wordText(lastIdx);
      const int rightHang = expectedRightOpticalHang(renderer, kFontId, lastWord, line.wordStyle(lastIdx));
      const int rightEdge =
          line.wordXpos(lastIdx) + renderer.getTextAdvanceX(kFontId, lastWord.c_str(), line.wordStyle(lastIdx));
      EXPECT_EQ(rightEdge, static_cast<int>(colW) + rightHang) << "Moby-Dick line=" << li << " col=" << colW;

      if (rightHang > 0) {
        if (line.wordHasDiscretionaryHyphen(lastIdx) || (!lastWord.empty() && lastWord.back() == '-')) {
          sawHyphenProtrusion = true;
        } else {
          sawPunctProtrusion = true;
        }
      }
    }
  }
  EXPECT_TRUE(sawHyphenProtrusion) << "Expected at least one hyphen optical hang in Moby-Dick Ch. 1";
  EXPECT_TRUE(sawPunctProtrusion) << "Expected at least one punctuation optical hang in Moby-Dick Ch. 1";

  // Dedicated optical hang checks: left opening quotes, right period & hyphen hang,
  // 0-px hang on non-protruding punctuation (':', ';'), non-justified alignments, last lines,
  // 0-gap single-word lines, and CJK punctuation.
  {
    ParsedText dialogue(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : {"\xE2\x80\x9CIf", "I", "made", "a", "serious", "mistake", "building", "modern", "tools."}) {
      dialogue.addWord(w, EpdFontFamily::REGULAR);
    }
    std::vector<std::unique_ptr<TextBlock>> lines;
    dialogue.layoutAndExtractLines(renderer, kFontId, 320,
                                   [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines[0]->wordXpos(0), -3);

    // Indented dialogue shifts from firstLineIndent (12 px) to 12 - 3 = 9 px; wide guillemet clamps to -3 px.
    ParsedText indented(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, BlockStyle{}, 2);
    for (const char* w : {"\xE2\x80\x9CIf", "modern", "readers", "align", "cleanly", "today."}) {
      indented.addWord(w, EpdFontFamily::REGULAR);
    }
    std::vector<std::unique_ptr<TextBlock>> indLines;
    indented.layoutAndExtractLines(renderer, kFontId, 260,
                                   [&](std::unique_ptr<TextBlock> b, uint32_t) { indLines.push_back(std::move(b)); });
    ASSERT_GE(indLines.size(), 2u);
    EXPECT_EQ(indLines[0]->wordXpos(0), 12 - 3);

    ParsedText guillemet(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : {"\xC2\xAB"
                          "Bonjour",
                          "tout", "le", "monde\xC2\xBB", "ici."}) {
      guillemet.addWord(w, EpdFontFamily::REGULAR);
    }
    std::vector<std::unique_ptr<TextBlock>> gmLines;
    guillemet.layoutAndExtractLines(renderer, kFontId, 250,
                                    [&](std::unique_ptr<TextBlock> b, uint32_t) { gmLines.push_back(std::move(b)); });
    ASSERT_GE(gmLines.size(), 2u);
    EXPECT_EQ(gmLines[0]->wordXpos(0), -3);
  }

  // Exact right optical hang on '.' (+3 px) and '-' (+3 px) vs 0 px on ';' / ':' / 0-gap single-word line.
  {
    renderer.ensureSdCardFontReady(kFontId, "When leader. industrial- note; said: arrived yesterday.", 0x01);
    const int whenPlusSp = renderer.getTextAdvanceX(kFontId, "When", EpdFontFamily::REGULAR) +
                           renderer.getSpaceAdvance(kFontId, 'n', 'l', EpdFontFamily::REGULAR);
    const int leaderDotW = renderer.getTextAdvanceX(kFontId, "leader.", EpdFontFamily::REGULAR);
    const uint16_t dotColW = static_cast<uint16_t>(whenPlusSp + leaderDotW - 1);

    ParsedText dotPara(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : {"When", "leader.", "arrived", "yesterday."}) dotPara.addWord(w, EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> dotLines;
    dotPara.layoutAndExtractLines(renderer, kFontId, dotColW,
                                  [&](std::unique_ptr<TextBlock> b, uint32_t) { dotLines.push_back(std::move(b)); });
    ASSERT_GE(dotLines.size(), 2u);
    ASSERT_EQ(dotLines[0]->wordCount(), 2u);
    EXPECT_STREQ(dotLines[0]->wordText(1), "leader.");
    EXPECT_EQ(dotLines[0]->wordXpos(1) + leaderDotW, static_cast<int>(dotColW) + 3);

    // Non-protruding semicolon must NOT protrude past dotColW.
    const int noteSemiW = renderer.getTextAdvanceX(kFontId, "note;", EpdFontFamily::REGULAR);
    const uint16_t semiColW = static_cast<uint16_t>(whenPlusSp + noteSemiW - 1);
    ParsedText semiPara(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : {"When", "note;", "arrived", "yesterday."}) semiPara.addWord(w, EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> semiLines;
    semiPara.layoutAndExtractLines(renderer, kFontId, semiColW,
                                   [&](std::unique_ptr<TextBlock> b, uint32_t) { semiLines.push_back(std::move(b)); });
    ASSERT_GE(semiLines.size(), 2u);
    const uint16_t semiLast = semiLines[0]->wordCount() - 1;
    EXPECT_LE(semiLines[0]->wordXpos(semiLast) + renderer.getTextAdvanceX(kFontId, semiLines[0]->wordText(semiLast),
                                                                          semiLines[0]->wordStyle(semiLast)),
              static_cast<int>(semiColW));

    // Hyphen optical hang on "indus-": fits 2 px narrower column and lands at indusColW + 3.
    const int indusHyphenW = renderer.getTextAdvanceX(kFontId, "indus-", EpdFontFamily::REGULAR);
    const uint16_t indusColW = static_cast<uint16_t>(whenPlusSp + indusHyphenW - 2);
    ParsedText hyphPara(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : {"When", "industrial", "design", "matters."}) hyphPara.addWord(w, EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> hyphLines;
    hyphPara.layoutAndExtractLines(renderer, kFontId, indusColW,
                                   [&](std::unique_ptr<TextBlock> b, uint32_t) { hyphLines.push_back(std::move(b)); });
    ASSERT_GE(hyphLines.size(), 2u);
    const uint16_t hLast = hyphLines[0]->wordCount() - 1;
    EXPECT_STREQ(hyphLines[0]->wordText(hLast), "indus-");
    EXPECT_TRUE(hyphLines[0]->wordHasDiscretionaryHyphen(hLast));
    EXPECT_EQ(hyphLines[0]->wordXpos(hLast) + indusHyphenW, static_cast<int>(indusColW) + 3);

    // 0-gap single-word line must NOT receive right optical hang in DP or extractLine.
    ParsedText singleDot(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    singleDot.addWord(
        "\xE2\x80\x9C"
        "brash.\xE2\x80\x9D",
        EpdFontFamily::REGULAR);
    singleDot.addWord("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> singleLines;
    singleDot.layoutAndExtractLines(
        renderer, kFontId, 508, [&](std::unique_ptr<TextBlock> b, uint32_t) { singleLines.push_back(std::move(b)); });
    ASSERT_GE(singleLines.size(), 2u);
    ASSERT_EQ(singleLines[0]->wordCount(), 1u);
    EXPECT_EQ(singleLines[0]->wordXpos(0), -3);
    EXPECT_LT(singleLines[0]->wordXpos(0) +
                  renderer.getTextAdvanceX(kFontId, singleLines[0]->wordText(0), EpdFontFamily::REGULAR),
              508);

    // Non-justified alignments and CJK punctuation must receive 0 px optical hang.
    for (const CssTextAlign align : {CssTextAlign::Left, CssTextAlign::Center, CssTextAlign::Right}) {
      BlockStyle nonJust = style;
      nonJust.alignment = align;
      ParsedText nj(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, nonJust, 0);
      for (const char* w : {"\xE2\x80\x9CIf", "I", "made", "a", "move", "in", "jail,\xE2\x80\x9D", "said", "he."}) {
        nj.addWord(w, EpdFontFamily::REGULAR);
      }
      std::vector<std::unique_ptr<TextBlock>> njLines;
      nj.layoutAndExtractLines(renderer, kFontId, 240,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { njLines.push_back(std::move(b)); });
      ASSERT_GE(njLines.size(), 2u);
      for (const auto& ln : njLines) {
        EXPECT_GE(ln->wordXpos(0), 0);
        const uint16_t li = ln->wordCount() - 1;
        EXPECT_LE(ln->wordXpos(li) + renderer.getTextAdvanceX(kFontId, ln->wordText(li), ln->wordStyle(li)), 240);
      }
    }

    ParsedText cjkPara(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    cjkPara.addWord(
        "\xE3\x80\x8C\xE4\xB8\xAD\xE6\x96\x87\xEF\xBC\x8C\xE4\xB8\xAD\xE6\x96\x87"
        "\xE3\x80\x82\xE3\x80\x8D\xE2\x80\x9C\xE4\xB8\xAD\xE6\x96\x87\xE2\x80\x9D"
        "\xE4\xB8\xAD\xE6\x96\x87",
        EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> cjkLines;
    cjkPara.layoutAndExtractLines(renderer, kFontId, 160,
                                  [&](std::unique_ptr<TextBlock> b, uint32_t) { cjkLines.push_back(std::move(b)); });
    ASSERT_GE(cjkLines.size(), 2u);
    for (size_t li = 0; li + 1 < cjkLines.size(); ++li) {
      EXPECT_EQ(cjkLines[li]->wordXpos(0), 0);
      const uint16_t last = cjkLines[li]->wordCount() - 1;
      EXPECT_EQ(cjkLines[li]->wordXpos(last) +
                    renderer.getTextAdvanceX(kFontId, cjkLines[li]->wordText(last), cjkLines[li]->wordStyle(last)),
                160);
    }
  }

  // Negative left optical hang survives serialization, link spans, framebuffer render, and v3 render signature.
  {
    constexpr int kMinMargin = 5;
    constexpr uint16_t kViewportW = 528 - 2 * kMinMargin;
    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    const uint8_t linkId = para.addLinkTarget("#note-1");
    ASSERT_GT(linkId, 0u);
    para.addWord("\xE2\x80\x9CIf", EpdFontFamily::REGULAR, false, false, 0, linkId);
    for (const char* w : {"modern", "portable", "readers", "render", "classic", "serif", "prose", "with", "careful",
                          "spacing", "and", "clean", "margins", "throughout", "every", "chapter."}) {
      para.addWord(w, EpdFontFamily::REGULAR);
    }
    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, kViewportW,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);
    const auto links = lines[0]->takeLinkSpans();
    ASSERT_EQ(links.size(), 1u);
    EXPECT_EQ(links[0].x, -3);

    std::vector<uint8_t> bytes;
    {
      HalFile wf;
      wf.openScratchBuffer(bytes, /*writeMode=*/true);
      ASSERT_TRUE(lines[0]->serialize(wf));
    }
    HalFile rf;
    rf.openScratchBuffer(bytes, /*writeMode=*/false);
    auto restored = TextBlock::deserialize(rf);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->wordXpos(0), -3);
    for (uint16_t i = 0; i + 1 < restored->wordCount(); ++i) {
      EXPECT_GT(restored->wordXpos(i + 1), restored->wordXpos(i));
    }

    renderer.clearScreen(0xFF);
    {
      auto scope = cacheMgr->createPrewarmScope();
      restored->render(renderer, kFontId, kMinMargin, 20);
      scope.endScanAndPrewarm();
      restored->render(renderer, kFontId, kMinMargin, 20);
    }
    const auto drawnCols = scanDrawnColumnsAtRow(renderer, 39, 528);
    ASSERT_FALSE(drawnCols.empty());
    EXPECT_EQ(drawnCols.front(), kMinMargin - 3);
    EXPECT_LT(drawnCols.back(), 528);

    // LAYOUT_SIGNATURE_VERSION == 3 must differ from v2.
    ReaderRenderSpec spec;
    spec.fontId = kFontId;
    spec.viewportWidth = kViewportW;
    spec.viewportHeight = 782;
    spec.hyphenationEnabled = true;
    uint32_t sigV2 = 2166136261U;
    const auto mixV2 = [&sigV2](const uint32_t value) {
      sigV2 ^= value;
      sigV2 *= 16777619U;
    };
    mixV2(2U);
    mixV2(static_cast<uint32_t>(spec.fontId));
    mixV2(static_cast<uint32_t>(spec.lineCompression * 1000.0f));
    mixV2(spec.extraParagraphSpacing);
    mixV2(static_cast<uint8_t>(spec.characterSpacing));
    mixV2(spec.wordSpacingPercent);
    mixV2(spec.paragraphAlignment);
    mixV2(spec.viewportWidth);
    mixV2(spec.viewportHeight);
    mixV2(spec.hyphenationEnabled);
    mixV2(spec.embeddedStyle);
    mixV2(spec.imageRendering);
    mixV2(spec.focusReadingEnabled);
    if (sigV2 == 0) sigV2 = 1;
    EXPECT_NE(readerRenderSpecSignature(spec), sigV2);
  }
}

// ============================================================================
// 18% Space Shrink Cap, 1.5x Asymmetry & Runt Last-Line Rebalancing
// ============================================================================

TEST_F(TypographyLayoutFixture, EighteenPercentSpaceShrinkCapAndAsymmetricBadness) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  renderer.ensureSdCardFontReady(
      kFontId, "When brash bold firm keen men came. met it modern readers. I saw a in big red old car port. Watery.",
      0x01);

  // 1. Exact 18% shrink cap: 6 words -> 5 spaces * 6 px = 30 px -> cap = 5 px (83.3% >= 82%).
  const char* kSixWords[] = {"When", "brash", "bold", "firm", "keen", "men"};
  int naturalSixW = 0;
  {
    ParsedText probe(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : kSixWords) probe.addWord(w, EpdFontFamily::REGULAR);
    probe.layoutAndExtractLines(renderer, kFontId, 800, [&](std::unique_ptr<TextBlock> b, uint32_t) {
      const uint16_t last = b->wordCount() - 1;
      naturalSixW = b->wordXpos(last) + renderer.getTextAdvanceX(kFontId, b->wordText(last), b->wordStyle(last));
    });
  }
  ASSERT_GT(naturalSixW, 200);

  // 5 px shrink (16.7% <= 18%): accepted; every space >= 5 px and sum >= 82% of 30 px.
  {
    ParsedText fit18(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : kSixWords) fit18.addWord(w, EpdFontFamily::REGULAR);
    for (const char* w : {"came.", "When", "bold"}) fit18.addWord(w, EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> lines;
    fit18.layoutAndExtractLines(renderer, kFontId, static_cast<uint16_t>(naturalSixW - 5),
                                [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines[0]->wordCount(), 6u);
    int compressedSpaceSum = 0;
    for (uint16_t wi = 0; wi + 1 < lines[0]->wordCount(); ++wi) {
      const int wordW = renderer.getTextAdvanceX(kFontId, lines[0]->wordText(wi), lines[0]->wordStyle(wi));
      const int gap = lines[0]->wordXpos(wi + 1) - (lines[0]->wordXpos(wi) + wordW);
      EXPECT_GE(gap, 5);
      compressedSpaceSum += gap;
    }
    EXPECT_GE(compressedSpaceSum * 100, 30 * 82);
  }

  // 6 px shrink (20.0% > 18%): rejected; word 6 wraps to line 1.
  {
    ParsedText reject20(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : kSixWords) reject20.addWord(w, EpdFontFamily::REGULAR);
    for (const char* w : {"came.", "When", "bold"}) reject20.addWord(w, EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> lines;
    reject20.layoutAndExtractLines(renderer, kFontId, static_cast<uint16_t>(naturalSixW - 6),
                                   [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines[0]->wordCount(), 5u);
  }

  // 2. 1.5x shrink-vs-stretch asymmetry prefers gentle 7-word stretch over full -18% 8-word shrink.
  {
    const char* kEightWords[] = {"When", "brash", "bold", "firm", "keen", "men", "met", "it"};
    int w8 = 0;
    {
      ParsedText p8(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
      for (const char* w : kEightWords) p8.addWord(w, EpdFontFamily::REGULAR);
      p8.layoutAndExtractLines(renderer, kFontId, 900, [&](std::unique_ptr<TextBlock> b, uint32_t) {
        const uint16_t last = b->wordCount() - 1;
        w8 = b->wordXpos(last) + renderer.getTextAdvanceX(kFontId, b->wordText(last), b->wordStyle(last));
      });
    }
    ParsedText asymPara(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : kEightWords) asymPara.addWord(w, EpdFontFamily::REGULAR);
    asymPara.addWord("modern", EpdFontFamily::REGULAR);
    asymPara.addWord("readers.", EpdFontFamily::REGULAR);

    std::vector<std::unique_ptr<TextBlock>> lines;
    asymPara.layoutAndExtractLines(renderer, kFontId, static_cast<uint16_t>(w8 - 7),
                                   [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0]->wordCount(), 7u);
    EXPECT_STREQ(lines[1]->wordText(0), "it");
  }

  // 3. Runt last-line (<20%) penalty pulls "a" down onto final line while sparing single-line paragraphs.
  {
    const char* kPrefixWithA[] = {"I", "saw", "a", "man", "in", "a", "big", "red", "old", "car", "a"};
    int wWithA = 0;
    {
      ParsedText probe(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
      for (const char* w : kPrefixWithA) probe.addWord(w, EpdFontFamily::REGULAR);
      probe.layoutAndExtractLines(renderer, kFontId, 900, [&](std::unique_ptr<TextBlock> b, uint32_t) {
        const uint16_t last = b->wordCount() - 1;
        wWithA = b->wordXpos(last) + renderer.getTextAdvanceX(kFontId, b->wordText(last), b->wordStyle(last));
      });
    }
    const uint16_t colW = static_cast<uint16_t>(wWithA - 5);
    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : kPrefixWithA) para.addWord(w, EpdFontFamily::REGULAR);
    para.addWord("port.", EpdFontFamily::REGULAR);

    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, colW,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_EQ(lines.size(), 2u);
    ASSERT_EQ(lines[1]->wordCount(), 2u);
    EXPECT_STREQ(lines[1]->wordText(0), "a");
    EXPECT_STREQ(lines[1]->wordText(1), "port.");

    for (const char* shortPara : {"it.", "Watery."}) {
      ParsedText single(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
      single.addWord(shortPara, EpdFontFamily::REGULAR);
      std::vector<std::unique_ptr<TextBlock>> sLines;
      single.layoutAndExtractLines(renderer, kFontId, 518,
                                   [&](std::unique_ptr<TextBlock> b, uint32_t) { sLines.push_back(std::move(b)); });
      ASSERT_EQ(sLines.size(), 1u);
      EXPECT_STREQ(sLines[0]->wordText(0), shortPara);
      EXPECT_FALSE(sLines[0]->wordHasDiscretionaryHyphen(0));
    }
  }
}

// ============================================================================
// NBSP Non-Shrink & Carry-Over
// ============================================================================

TEST_F(TypographyLayoutFixture, NbspNonShrinkAndCarryOver) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  // 1. Overflowing justified NBSP line keeps monotonic wordXpos and never compresses NBSP below natural width.
  {
    ParsedText para(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    para.addWord("100", EpdFontFamily::REGULAR);
    para.addWord(" ", EpdFontFamily::REGULAR, /*underline=*/false, /*attachToPrevious=*/true);
    para.addWord("9876543210987654", EpdFontFamily::REGULAR, /*underline=*/false, /*attachToPrevious=*/true);
    para.addWord("next.", EpdFontFamily::REGULAR);

    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, 220,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_EQ(lines.size(), 2u);
    ASSERT_EQ(lines[0]->wordCount(), 3u);
    for (uint16_t i = 0; i + 1 < lines[0]->wordCount(); ++i) {
      EXPECT_GT(lines[0]->wordXpos(i + 1), lines[0]->wordXpos(i));
    }
    const int w100 = renderer.getTextAdvanceX(kFontId, "100", EpdFontFamily::REGULAR);
    const int naturalSpace = renderer.getSpaceWidth(kFontId, EpdFontFamily::REGULAR);
    EXPECT_EQ(lines[0]->wordXpos(1), lines[0]->wordXpos(0) + w100);
    EXPECT_EQ(lines[0]->wordXpos(2) - lines[0]->wordXpos(1), naturalSpace);
  }

  // 2. NBSP carry-over keeps the glued pair together on the next line rather than breaking at NBSP.
  {
    renderer.ensureSdCardFontReady(kFontId, "modern readers 100 200 continue.", 0x01);
    const int wMod = renderer.getTextAdvanceX(kFontId, "modern", EpdFontFamily::REGULAR);
    const int wRead = renderer.getTextAdvanceX(kFontId, "readers", EpdFontFamily::REGULAR);
    const int w100 = renderer.getTextAdvanceX(kFontId, "100", EpdFontFamily::REGULAR);
    const uint16_t colW = static_cast<uint16_t>(wMod + 6 + wRead + 6 + w100 + 2);

    ParsedText para(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    para.addWord("modern", EpdFontFamily::REGULAR);
    para.addWord("readers", EpdFontFamily::REGULAR);
    para.addWord("100", EpdFontFamily::REGULAR);
    para.addWord(" ", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true);
    para.addWord("200", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true);
    para.addWord("continue.", EpdFontFamily::REGULAR);

    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, colW,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_EQ(lines.size(), 2u);
    ASSERT_EQ(lines[0]->wordCount(), 2u);
    EXPECT_STREQ(lines[0]->wordText(1), "readers");
    ASSERT_GE(lines[1]->wordCount(), 3u);
    EXPECT_STREQ(lines[1]->wordText(0), "100");
    EXPECT_STREQ(lines[1]->wordText(1), " ");
    EXPECT_STREQ(lines[1]->wordText(2), "200");
  }

  // 3. Emergency pre-split on overflowing NBSP-glued groups ("When water" and "When through").
  {
    renderer.ensureSdCardFontReady(kFontId, "When water wa- through safely.", 0x01);
    const int whenW = renderer.getTextAdvanceX(kFontId, "When", EpdFontFamily::REGULAR);
    const int waPrefixW = renderer.getTextAdvanceX(kFontId, "wa-", EpdFontFamily::REGULAR);
    const uint16_t waterColW = static_cast<uint16_t>(whenW + 6 + waPrefixW + 2);

    ParsedText waterNbsp(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    waterNbsp.addWord("When", EpdFontFamily::REGULAR);
    waterNbsp.addWord(" ", EpdFontFamily::REGULAR, false, true);
    waterNbsp.addWord("water", EpdFontFamily::REGULAR, false, true);
    std::vector<std::unique_ptr<TextBlock>> wLines;
    waterNbsp.layoutAndExtractLines(renderer, kFontId, waterColW,
                                    [&](std::unique_ptr<TextBlock> b, uint32_t) { wLines.push_back(std::move(b)); });
    ASSERT_EQ(wLines.size(), 2u);
    EXPECT_STREQ(wLines[0]->wordText(wLines[0]->wordCount() - 1), "wa-");
    EXPECT_STREQ(wLines[1]->wordText(0), "ter");

    const int throughW = renderer.getTextAdvanceX(kFontId, "through", EpdFontFamily::REGULAR);
    const uint16_t throughColW = static_cast<uint16_t>(whenW + 6 + throughW - 2);
    ParsedText throughNbsp(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    throughNbsp.addWord("When", EpdFontFamily::REGULAR);
    throughNbsp.addWord(" ", EpdFontFamily::REGULAR, false, true);
    throughNbsp.addWord("through", EpdFontFamily::REGULAR, false, true);
    throughNbsp.addWord("safely.", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> tLines;
    throughNbsp.layoutAndExtractLines(renderer, kFontId, throughColW,
                                      [&](std::unique_ptr<TextBlock> b, uint32_t) { tLines.push_back(std::move(b)); });
    ASSERT_GE(tLines.size(), 2u);
    EXPECT_TRUE(tLines[0]->wordHasDiscretionaryHyphen(tLines[0]->wordCount() - 1));
    for (size_t li = 0; li < tLines.size(); ++li) {
      const uint16_t last = tLines[li]->wordCount() - 1;
      const int rightEdge = tLines[li]->wordXpos(last) +
                            renderer.getTextAdvanceX(kFontId, tLines[li]->wordText(last), tLines[li]->wordStyle(last));
      EXPECT_LE(rightEdge, static_cast<int>(throughColW) + (tLines[li]->wordHasDiscretionaryHyphen(last) ? 3 : 0));
    }
  }
}

// ============================================================================
// 6-Letter Floor, Explicit U+00AD Soft Hyphen & Hangul Line-End Breaks
// ============================================================================

TEST_F(TypographyLayoutFixture, SixLetterFloorSoftHyphenAndHangulBreaks) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  renderer.ensureSdCardFontReady(
      kFontId, "When clear watery mobile nothing water paper never about wa- mo- noth- observations study.", 0x01);
  const int prefixBaseW = renderer.getTextAdvanceX(kFontId, "When", EpdFontFamily::REGULAR) +
                          renderer.getSpaceAdvance(kFontId, 'n', 'c', EpdFontFamily::REGULAR) +
                          renderer.getTextAdvanceX(kFontId, "clear", EpdFontFamily::REGULAR) + 6;

  // 1. 6+ letter words hyphenate normally in KP DP.
  const char* kSixPlus[] = {"watery", "mobile", "nothing"};
  const char* kPrefixes[] = {"wa-", "mo-", "noth-"};
  for (size_t i = 0; i < std::size(kSixPlus); ++i) {
    SCOPED_TRACE(kSixPlus[i]);
    const uint16_t colW =
        static_cast<uint16_t>(prefixBaseW + renderer.getTextAdvanceX(kFontId, kPrefixes[i], EpdFontFamily::REGULAR));
    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : {"When", "clear", kSixPlus[i], "observations", "study."})
      para.addWord(w, EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, colW,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);
    const uint16_t last0 = lines[0]->wordCount() - 1;
    EXPECT_TRUE(lines[0]->wordHasDiscretionaryHyphen(last0));
    EXPECT_STREQ(lines[0]->wordText(last0), kPrefixes[i]);
  }

  // 2. 5-letter words stay whole in both DP and greedy (maxLines=1), whereas explicit U+00AD is honored.
  const int waPrefixW = renderer.getTextAdvanceX(kFontId, "wa-", EpdFontFamily::REGULAR);
  const uint16_t fiveColW = static_cast<uint16_t>(prefixBaseW + waPrefixW + 4);
  for (const char* fiveLetter : {"water", "paper", "never", "about"}) {
    for (const size_t maxLines : {SIZE_MAX, size_t{1}}) {
      ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
      for (const char* w : {"When", "clear", fiveLetter, "observations", "study."}) {
        para.addWord(w, EpdFontFamily::REGULAR);
      }
      std::vector<std::unique_ptr<TextBlock>> lines;
      para.layoutAndExtractLines(
          renderer, kFontId, fiveColW, [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); },
          true, 0, 100, maxLines);
      ASSERT_GE(lines.size(), 1u);
      const uint16_t last0 = lines[0]->wordCount() - 1;
      EXPECT_FALSE(lines[0]->wordHasDiscretionaryHyphen(last0));
      EXPECT_STREQ(lines[0]->wordText(last0), "clear");
    }
  }

  {
    ParsedText shyPara(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : {"When", "clear", "wa\xC2\xADter", "observations", "study."}) {
      shyPara.addWord(w, EpdFontFamily::REGULAR);
    }
    std::vector<std::unique_ptr<TextBlock>> shyLines;
    shyPara.layoutAndExtractLines(renderer, kFontId, fiveColW,
                                  [&](std::unique_ptr<TextBlock> b, uint32_t) { shyLines.push_back(std::move(b)); });
    ASSERT_GE(shyLines.size(), 2u);
    EXPECT_STREQ(shyLines[0]->wordText(shyLines[0]->wordCount() - 1), "wa-");
    EXPECT_STREQ(shyLines[1]->wordText(0), "ter");
  }

  // 3. Emergency overflow fallback splits a 5-letter word wider than the column.
  {
    const int waterFullW = renderer.getTextAdvanceX(kFontId, "water", EpdFontFamily::REGULAR);
    const uint16_t narrowColW = static_cast<uint16_t>(waterFullW - 4);
    ASSERT_GE(narrowColW, static_cast<uint16_t>(waPrefixW));
    ParsedText narrowPara(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    narrowPara.addWord("water", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> lines;
    narrowPara.layoutAndExtractLines(renderer, kFontId, narrowColW,
                                     [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_TRUE(lines[0]->wordHasDiscretionaryHyphen(0));
  }

  // 4. Hangul words (< 6 syllables) split cleanly at syllable boundaries without inserting ASCII '-'.
  {
    // Each Hangul syllable = 18 px, space = 6 px. "가나다"(54) + space(6) + "대한"(36) = 96 px exact fit.
    ParsedText hangulPara(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    hangulPara.addWord("가나다", EpdFontFamily::REGULAR);
    hangulPara.addWord("대한민국", EpdFontFamily::REGULAR);
    hangulPara.addWord("컴퓨터", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> lines;
    hangulPara.layoutAndExtractLines(renderer, kFontId, 96,
                                     [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);
    const uint16_t last0 = lines[0]->wordCount() - 1;
    EXPECT_STREQ(lines[0]->wordText(last0), "대한");
    EXPECT_FALSE(lines[0]->wordHasDiscretionaryHyphen(last0));
    EXPECT_STREQ(lines[1]->wordText(0), "민국");
    EXPECT_EQ(reconstructParagraph(lines), "가나다대한민국컴퓨터");
  }
}

// ============================================================================
// U+2011 Non-Breaking Hyphen Protection
// ============================================================================

TEST_F(TypographyLayoutFixture, NonBreakingHyphenU2011PreventsBreak) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  const std::string nbCompound = "well\xE2\x80\x91known";
  renderer.ensureSdCardFontReady(kFontId, "When modern well-known well\xE2\x80\x91known readers study.", 0x01);

  const int prefixW = renderer.getTextAdvanceX(kFontId, "When", EpdFontFamily::REGULAR) +
                      renderer.getSpaceAdvance(kFontId, 'n', 'm', EpdFontFamily::REGULAR) +
                      renderer.getTextAdvanceX(kFontId, "modern", EpdFontFamily::REGULAR) +
                      renderer.getSpaceAdvance(kFontId, 'n', 'w', EpdFontFamily::REGULAR);
  const int wellDashW = renderer.getTextAdvanceX(kFontId, "well-", EpdFontFamily::REGULAR);
  const uint16_t colW = static_cast<uint16_t>(prefixW + wellDashW + 4);

  // Confirm ASCII '-' breaks cleanly at the exact same column width.
  {
    ParsedText asciiPara(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : {"When", "modern", "well-known", "readers", "study."}) {
      asciiPara.addWord(w, EpdFontFamily::REGULAR);
    }
    std::vector<std::unique_ptr<TextBlock>> lines;
    asciiPara.layoutAndExtractLines(renderer, kFontId, colW,
                                    [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);
    EXPECT_STREQ(lines[0]->wordText(lines[0]->wordCount() - 1), "well-");
    EXPECT_STREQ(lines[1]->wordText(0), "known");
  }

  // U+2011 must NEVER break after U+2011 in either KP DP (SIZE_MAX) or greedy (maxLines=1).
  for (const size_t maxLines : {SIZE_MAX, size_t{1}}) {
    SCOPED_TRACE(maxLines);
    ParsedText nbPara(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    nbPara.addWord("When", EpdFontFamily::REGULAR);
    nbPara.addWord("modern", EpdFontFamily::REGULAR);
    nbPara.addWord(nbCompound, EpdFontFamily::REGULAR);
    nbPara.addWord("readers", EpdFontFamily::REGULAR);
    nbPara.addWord("study.", EpdFontFamily::REGULAR);

    std::vector<std::unique_ptr<TextBlock>> lines;
    nbPara.layoutAndExtractLines(
        renderer, kFontId, colW, [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); }, true,
        0, 100, maxLines);
    ASSERT_GE(lines.size(), 1u);
    const std::string last0 = lines[0]->wordText(lines[0]->wordCount() - 1);
    EXPECT_EQ(last0, "modern") << "U+2011 compound split unexpectedly on line 0: " << last0;
    if (lines.size() >= 2u) {
      EXPECT_EQ(std::string(lines[1]->wordText(0)), nbCompound);
    }
  }
}

// ============================================================================
// <ruby> Overhang Preservation & Multi-Word Ruby Groups
// ============================================================================

TEST_F(TypographyLayoutFixture, RubyOverhangPreservationAndMultiWordRubyGroups) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  renderer.ensureSdCardFontReady(kFontId,
                                 "When a. \xE2\x80\x9C"
                                 "a MMMMMM Pref testing next line. grp",
                                 0x01);

  // 1. Wide ruby on the first word protects left margin (extraStartOffset > 0) and disables negative left hang.
  {
    const std::string qBase =
        "\xE2\x80\x9C"
        "a";
    const int baseW = renderer.getTextAdvanceX(kFontId, qBase.c_str(), EpdFontFamily::REGULAR);
    const int rubyW = renderer.getTextAdvanceX(kFontId, "MMMMMM", EpdFontFamily::SUP);
    ASSERT_GT(rubyW, baseW);
    const int expectedStartOffset = (rubyW - baseW) / 2;

    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    para.addWord(qBase, EpdFontFamily::REGULAR);
    para.setRubyForWordAt(0, "MMMMMM");
    for (const char* w : {"modern", "readers", "study", "carefully."}) para.addWord(w, EpdFontFamily::REGULAR);

    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, 240,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines[0]->wordXpos(0), expectedStartOffset);
    ASSERT_FALSE(lines[0]->getRubyTexts().empty());
    EXPECT_EQ(lines[0]->getRubyTexts()[0], "MMMMMM");
  }

  // 2. Wide ruby on the last word of a justified line reserves extraEndOffset and suppresses right optical hang.
  {
    const int whenW = renderer.getTextAdvanceX(kFontId, "When", EpdFontFamily::REGULAR);
    const int baseDotW = renderer.getTextAdvanceX(kFontId, "a.", EpdFontFamily::REGULAR);
    const int rubyW = renderer.getTextAdvanceX(kFontId, "MMMMMM", EpdFontFamily::SUP);
    const int expectedEndOffset = (rubyW - baseDotW + 1) / 2;
    ASSERT_GT(expectedEndOffset, 0);
    const uint16_t colW = static_cast<uint16_t>(whenW + 6 + rubyW + expectedEndOffset + 20);

    ParsedText para(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    para.addWord("When", EpdFontFamily::REGULAR);
    para.addWord("a.", EpdFontFamily::REGULAR);
    para.setRubyForWordAt(1, "MMMMMM");
    para.addWord("internationalization", EpdFontFamily::REGULAR);
    para.addWord("line.", EpdFontFamily::REGULAR);

    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, colW,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);
    ASSERT_EQ(lines[0]->wordCount(), 2u);
    const int baseRightEdge = lines[0]->wordXpos(1) + baseDotW;
    // Extra end offset keeps the ruby overhang strictly inside colW (no right optical hang past colW).
    EXPECT_LE(baseRightEdge + expectedEndOffset, static_cast<int>(colW));
  }

  // 3. Multi-word ruby group stays intact with RUBY_CONTINUE preserved and is not hyphenated by KP DP.
  {
    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    para.addWord("Pref", EpdFontFamily::REGULAR);
    para.addWord("testing", EpdFontFamily::REGULAR);
    para.setRubyGroupAt(0, 2, "grp");
    para.addWord("next", EpdFontFamily::REGULAR);
    para.addWord("word.", EpdFontFamily::REGULAR);

    const int grpW = renderer.getTextAdvanceX(kFontId, "Pref", EpdFontFamily::REGULAR) +
                     renderer.getTextAdvanceX(kFontId, "testing", EpdFontFamily::REGULAR);
    const uint16_t colW = static_cast<uint16_t>(grpW + 16);

    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, colW,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);
    ASSERT_EQ(lines[0]->wordCount(), 2u);
    EXPECT_STREQ(lines[0]->wordText(0), "Pref");
    EXPECT_STREQ(lines[0]->wordText(1), "testing");
    EXPECT_FALSE(lines[0]->wordHasDiscretionaryHyphen(1));
    EXPECT_NE(lines[0]->wordStyle(1) & EpdFontFamily::RUBY_CONTINUE, 0);
    ASSERT_FALSE(lines[0]->getRubyTexts().empty());
    EXPECT_EQ(lines[0]->getRubyTexts()[0], "grp");
  }
}

// ============================================================================
// Focus Reading Bold Splits
// ============================================================================

TEST_F(TypographyLayoutFixture, FocusReadingBoldSplitsMaintainAlignmentAndBoundaries) {
  buildBookerly14CpFont(/*includeBold=*/true);
  SdCardFont boldFont;
  ASSERT_TRUE(boldFont.load("/fonts/BookerlyBold_14.cpfont"));
  ASSERT_TRUE(boldFont.hasStyle(EpdFontFamily::BOLD));
  constexpr int kBoldFontId = 88;
  renderer.insertFont(kBoldFontId, EpdFontFamily(14, boldFont.getEpdFont(0), boldFont.getEpdFont(1)));
  renderer.registerSdCardFont(kBoldFontId, &boldFont);

  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  // 1. Justified paragraph with Focus Reading + hyphenation aligns flush right with 0 px drift.
  {
    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/true, style, 0);
    const char* words[] = {"When",   "modern", "portable", "readers", "render",  "classic", "serif",    "typography",
                           "across", "narrow", "columns,", "careful", "spacing", "between", "adjacent", "entrepreneur",
                           "words",  "keeps",  "every",    "margin",  "clean",   "and",     "steady."};
    for (const char* w : words) para.addWord(w, EpdFontFamily::REGULAR);

    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kBoldFontId, 410,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 3u);

    bool sawFocusPrefix = false;
    for (size_t li = 0; li + 1 < lines.size(); ++li) {
      const TextBlock& line = *lines[li];
      ASSERT_GT(line.wordCount(), 1u);
      for (uint16_t wi = 0; wi < line.wordCount(); ++wi) {
        if (line.focusBoundary(wi) > 0) sawFocusPrefix = true;
      }
      const uint16_t lastIdx = line.wordCount() - 1;
      const int rightHang =
          expectedRightOpticalHang(renderer, kBoldFontId, line.wordText(lastIdx), line.wordStyle(lastIdx));
      const int rightEdge = line.wordXpos(lastIdx) + measuredBlockWordWidth(renderer, kBoldFontId, line, lastIdx);
      EXPECT_EQ(rightEdge, 410 + rightHang) << "Focus Reading drift on line=" << li;
    }
    EXPECT_TRUE(sawFocusPrefix);
  }

  // 2. Splitting a focused word before its bold boundary promotes the prefix (plus inserted '-')
  //    to all-BOLD with focusBoundary == 0 and carries the remaining bold prefix onto the suffix.
  {
    renderer.ensureSdCardFontReady(kBoldFontId, "When modern en- trepreneur readers study.", 0x03);
    ParsedText probe(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/true, style, 0);
    probe.addWord("When", EpdFontFamily::REGULAR);
    probe.addWord("modern", EpdFontFamily::REGULAR);
    int prefixW = 0;
    probe.layoutAndExtractLines(renderer, kBoldFontId, 600, [&](std::unique_ptr<TextBlock> b, uint32_t) {
      prefixW = b->wordXpos(1) + measuredBlockWordWidth(renderer, kBoldFontId, *b, 1) + 6;
    });
    const int boldEnHyphenW = renderer.getTextAdvanceX(kBoldFontId, "en-", EpdFontFamily::BOLD);
    const uint16_t colW = static_cast<uint16_t>(prefixW + boldEnHyphenW);

    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/true, style, 0);
    for (const char* w : {"When", "modern", "entrepreneur", "readers", "study."}) {
      para.addWord(w, EpdFontFamily::REGULAR);
    }
    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kBoldFontId, colW,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);
    const uint16_t last0 = lines[0]->wordCount() - 1;
    EXPECT_STREQ(lines[0]->wordText(last0), "en-");
    EXPECT_TRUE(lines[0]->wordHasDiscretionaryHyphen(last0));
    EXPECT_NE(lines[0]->wordStyle(last0) & EpdFontFamily::BOLD, 0);
    EXPECT_EQ(lines[0]->focusBoundary(last0), 0u);
    EXPECT_STREQ(lines[1]->wordText(0), "trepreneur");
    EXPECT_GT(lines[1]->focusBoundary(0), 0u);
  }
}

// ============================================================================
// Bounded maxLines (Drop-Cap Path) & Streaming includeLastLine=false
// ============================================================================

TEST_F(TypographyLayoutFixture, MaxLinesDropCapLeavesRemainingWordsUnsplit) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  for (const bool hyphenate : {false, true}) {
    SCOPED_TRACE(hyphenate);
    ParsedText para(hyphenate, /*focusReadingEnabled=*/false, style, 0);
    const char* words[] = {"Opening", "lines", "beside", "a", "drop", "cap", "internationalization", "storytelling."};
    for (const char* w : words) para.addWord(w, EpdFontFamily::REGULAR);

    std::vector<std::unique_ptr<TextBlock>> dropCapLines;
    para.layoutAndExtractLines(
        renderer, kFontId, /*viewportWidth=*/170,
        [&](std::unique_ptr<TextBlock> b, uint32_t) { dropCapLines.push_back(std::move(b)); },
        /*includeLastLine=*/true, /*characterSpacing=*/0, /*wordSpacingPercent=*/100, /*maxLines=*/2);
    ASSERT_EQ(dropCapLines.size(), 2u);
    ASSERT_FALSE(para.isEmpty());

    std::vector<std::unique_ptr<TextBlock>> fullLines;
    para.layoutAndExtractLines(renderer, kFontId, /*viewportWidth=*/680,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { fullLines.push_back(std::move(b)); });
    ASSERT_EQ(fullLines.size(), 1u);
    EXPECT_TRUE(para.isEmpty());
    // "internationalization" and "storytelling." were beyond maxLines=2 and must stay intact.
    bool sawIntactLongWord = false;
    for (uint16_t wi = 0; wi < fullLines[0]->wordCount(); ++wi) {
      if (std::string(fullLines[0]->wordText(wi)) == "internationalization") {
        sawIntactLongWord = true;
        EXPECT_FALSE(fullLines[0]->wordHasDiscretionaryHyphen(wi));
      }
    }
    EXPECT_TRUE(sawIntactLongWord);

    std::vector<std::unique_ptr<TextBlock>> combined;
    for (auto& l : dropCapLines) combined.push_back(std::move(l));
    for (auto& l : fullLines) combined.push_back(std::move(l));
    EXPECT_EQ(reconstructParagraph(combined), "Openinglinesbesideadropcapinternationalizationstorytelling.");
  }
}

TEST_F(TypographyLayoutFixture, StreamingIncludeLastLineFalsePreservesMidWordSplits) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = false;

  renderer.ensureSdCardFontReady(kFontId, "When modern internationalization in- continues smoothly.", 0x01);
  const int prefixW = renderer.getTextAdvanceX(kFontId, "When", EpdFontFamily::REGULAR) +
                      renderer.getSpaceAdvance(kFontId, 'n', 'm', EpdFontFamily::REGULAR) +
                      renderer.getTextAdvanceX(kFontId, "modern", EpdFontFamily::REGULAR) + 6;
  const int indentW = 2 * renderer.getSpaceWidth(kFontId, EpdFontFamily::REGULAR);
  const int interHyphenW = renderer.getTextAdvanceX(kFontId, "inter-", EpdFontFamily::REGULAR);
  const uint16_t colW = static_cast<uint16_t>(indentW + prefixW + interHyphenW + 4);

  ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, /*paragraphIndentSpaces=*/2);
  para.addWord("When", EpdFontFamily::REGULAR);
  para.addWord("modern", EpdFontFamily::REGULAR);
  para.addWord("internationalization", EpdFontFamily::REGULAR);

  std::vector<std::unique_ptr<TextBlock>> streamedLines;
  std::vector<int> firstCallRemaining;
  para.layoutAndExtractLines(
      renderer, kFontId, colW,
      [&](std::unique_ptr<TextBlock> b, uint32_t) {
        firstCallRemaining.push_back(para.linesAfterCurrentLine());
        streamedLines.push_back(std::move(b));
      },
      /*includeLastLine=*/false);

  ASSERT_EQ(streamedLines.size(), 1u);
  EXPECT_EQ(firstCallRemaining[0], -1);
  EXPECT_EQ(streamedLines[0]->wordXpos(0), indentW);
  const uint16_t last0 = streamedLines[0]->wordCount() - 1;
  EXPECT_TRUE(streamedLines[0]->wordHasDiscretionaryHyphen(last0));
  ASSERT_FALSE(para.isEmpty());

  // Append continuation words and flush with includeLastLine=true.
  para.addWord("continues", EpdFontFamily::REGULAR);
  para.addWord("smoothly.", EpdFontFamily::REGULAR);

  std::vector<int> secondCallRemaining;
  const size_t beforeFlush = streamedLines.size();
  para.layoutAndExtractLines(
      renderer, kFontId, colW,
      [&](std::unique_ptr<TextBlock> b, uint32_t) {
        secondCallRemaining.push_back(para.linesAfterCurrentLine());
        streamedLines.push_back(std::move(b));
      },
      /*includeLastLine=*/true);

  EXPECT_TRUE(para.isEmpty());
  ASSERT_GT(streamedLines.size(), beforeFlush);
  // Continuation line must not re-apply the first-line indent.
  EXPECT_EQ(streamedLines[beforeFlush]->wordXpos(0), 0);
  EXPECT_EQ(secondCallRemaining.back(), 0);
  EXPECT_EQ(reconstructParagraph(streamedLines), "Whenmoderninternationalizationcontinuessmoothly.");
}

// ============================================================================
// >= 400-Word Greedy Fallback, 160..200 B / >220 B Words & OOM Split Fallback
// ============================================================================

TEST_F(TypographyLayoutFixture, LongParagraphOver400WordsUsesGreedyFallbackWithoutTruncation) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  static constexpr const char* kVocab[] = {
      "portable",   "electronic", "typography", "carefully",  "balances",     "interword",  "spacing",
      "throughout", "extended",   "literary",   "paragraphs", "without",      "truncating", "remaining",
      "sentences",  "across",     "narrow",     "columns",    "storytelling", "expertise",  "devices",
      "modern",     "readers",    "affluent",   "office",     "first",
  };

  for (const size_t wordCount : {size_t{450}, size_t{750}}) {
    for (const uint16_t colW : {uint16_t{190}, uint16_t{518}}) {
      SCOPED_TRACE(::testing::Message() << "words=" << wordCount << " colW=" << colW);
      ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
      std::string expectedConcat;
      for (size_t i = 0; i < wordCount; ++i) {
        const char* w = kVocab[i % std::size(kVocab)];
        para.addWord(w, EpdFontFamily::REGULAR);
        expectedConcat += w;
      }

      std::vector<std::unique_ptr<TextBlock>> lines;
      para.layoutAndExtractLines(renderer, kFontId, colW,
                                 [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
      EXPECT_TRUE(para.isEmpty());
      EXPECT_FALSE(para.hadDroppedWords());
      EXPECT_GT(lines.size(), 40u);
      EXPECT_EQ(reconstructParagraph(lines), expectedConcat);
    }
  }
}

TEST_F(TypographyLayoutFixture, CompoundWordsBetween160And200BytesAndOver220BytesSplitCleanly) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  // 1. 178-byte compound word with a soft hyphen at byte 166 (> FAST_MEASURE_BYTES=156, <= MAX_HYPHEN_WORD_BYTES=200)
  //    must split cleanly via KP DP and heap fallback measurement when column width fits the 166-byte prefix.
  {
    const std::string prefix164 = std::string(163, '0') + "a";
    ASSERT_EQ(prefix164.size(), 164u);
    const std::string compound180 = prefix164 + "\xC2\xAD" + "storytelling";
    ASSERT_GT(compound180.size(), 170u);
    ASSERT_LE(compound180.size(), 200u);

    renderer.ensureSdCardFontReady(kFontId, "internationalization 0a- storytelling done.", 0x01);
    const int leadSpW = renderer.getTextAdvanceX(kFontId, "internationalization", EpdFontFamily::REGULAR) + 6;
    const int preHyphenW = renderer.getTextAdvanceX(kFontId, (prefix164 + "-").c_str(), EpdFontFamily::REGULAR);
    const uint16_t wideColW = static_cast<uint16_t>(leadSpW + preHyphenW + 4);

    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    para.addWord("internationalization", EpdFontFamily::REGULAR);
    para.addWord(compound180, EpdFontFamily::REGULAR);
    para.addWord("done.", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, wideColW,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_EQ(lines.size(), 2u);
    const uint16_t last0 = lines[0]->wordCount() - 1;
    EXPECT_TRUE(lines[0]->wordHasDiscretionaryHyphen(last0));
    EXPECT_EQ(std::string(lines[0]->wordText(last0)), prefix164 + "-");
    EXPECT_STREQ(lines[1]->wordText(0), "storytelling");
    EXPECT_EQ(reconstructParagraph(lines), "internationalization" + prefix164 + "storytellingdone.");
  }

  // 2. Oversized hyphenatable words > 220 bytes pre-split cleanly without overflow.
  {
    std::string softWord;
    std::string expectedClean;
    for (int i = 0; i < 16; ++i) {
      if (i > 0) softWord += "\xC2\xAD";
      const char* seg = (i % 2 == 0) ? "international" : "storytelling";
      softWord += seg;
      expectedClean += seg;
    }
    ASSERT_GT(softWord.size(), 220u);

    for (const uint16_t colW : {uint16_t{180}, uint16_t{518}}) {
      ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
      para.addWord("See", EpdFontFamily::REGULAR);
      para.addWord(softWord, EpdFontFamily::REGULAR);
      std::vector<std::unique_ptr<TextBlock>> lines;
      para.layoutAndExtractLines(renderer, kFontId, colW,
                                 [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
      ASSERT_GE(lines.size(), 3u);
      EXPECT_TRUE(para.isEmpty());
      EXPECT_EQ(reconstructParagraph(lines), "See" + expectedClean);
      for (size_t li = 0; li < lines.size(); ++li) {
        const uint16_t last = lines[li]->wordCount() - 1;
        const int rightEdge = lines[li]->wordXpos(last) +
                              renderer.getTextAdvanceX(kFontId, lines[li]->wordText(last), lines[li]->wordStyle(last));
        EXPECT_LE(rightEdge, static_cast<int>(colW) + (li + 1 < lines.size() ? 3 : 0));
      }
    }
  }
}

TEST_F(TypographyLayoutFixture, SplitMaterializationOomFallbackProducesStrictlyIncreasingBreaks) {
  renderer.ensureSdCardFontReady(kFontId, "first characteristically oft.", 0x01);

  for (const bool includeLastLine : {true, false}) {
    SCOPED_TRACE(includeLastLine);
    BlockStyle style;
    style.alignment = CssTextAlign::Justify;
    style.textIndentDefined = true;
    style.textIndent = 0;

    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    for (int i = 0; i < 337; ++i) para.addWord("first", EpdFontFamily::REGULAR);
    para.layoutAndExtractLines(
        renderer, kFontId, 518, [](std::unique_ptr<TextBlock>, uint32_t) {}, /*includeLastLine=*/true);
    ASSERT_TRUE(para.isEmpty());
    para.resetFirstLineIndent();

    para.addWord("characteristically", EpdFontFamily::REGULAR, false, false, 0);
    para.addWord("oft.", EpdFontFamily::REGULAR, false, false, 19);

    gDeniedNothrowArrayAllocs = 0;
    gFailNothrowArrayAllocSize = 2048;  // Deny WordStore's fresh 2048 B chunk during splitWordAtOffset().
    std::vector<std::unique_ptr<TextBlock>> lines;
    std::vector<uint32_t> offsets;
    para.layoutAndExtractLines(
        renderer, kFontId, /*viewportWidth=*/95,
        [&](std::unique_ptr<TextBlock> b, const uint32_t visOff) {
          lines.push_back(std::move(b));
          offsets.push_back(visOff);
        },
        includeLastLine);
    gFailNothrowArrayAllocSize = 0;
    EXPECT_GT(gDeniedNothrowArrayAllocs, 0u);

    if (!includeLastLine) {
      para.layoutAndExtractLines(
          renderer, kFontId, /*viewportWidth=*/95,
          [&](std::unique_ptr<TextBlock> b, const uint32_t visOff) {
            lines.push_back(std::move(b));
            offsets.push_back(visOff);
          },
          /*includeLastLine=*/true);
    }

    ASSERT_EQ(lines.size(), 2u);
    for (size_t li = 0; li < lines.size(); ++li) {
      ASSERT_NE(lines[li], nullptr);
      EXPECT_TRUE(lines[li]->valid());
      EXPECT_GT(lines[li]->wordCount(), 0u);
      if (li > 0) EXPECT_GT(offsets[li], offsets[li - 1]);
    }
    EXPECT_STREQ(lines[0]->wordText(0), "characteristically");
    EXPECT_STREQ(lines[1]->wordText(0), "oft.");
  }
}

// ============================================================================
// Round 1 Firmware-Handoff Regression Tests
// ============================================================================

TEST_F(TypographyLayoutFixture, Over400AndPreSplitGrowthKeepDiscretionaryHyphensLineFinal) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  // 1. >= 400-word paragraph with an oversized hyphenatable word routes directly to greedy
  //    before pre-split and keeps DISCRETIONARY_HYPHEN_FLAG strictly line-final.
  {
    const std::string oversized =
        "international\xC2\xAD"
        "storytelling\xC2\xAD"
        "international\xC2\xAD"
        "storytelling";
    const std::string oversizedClean = "internationalstorytellinginternationalstorytelling";
    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    std::string expectedConcat;
    for (int i = 0; i < 405; ++i) {
      if (i == 200) {
        para.addWord(oversized, EpdFontFamily::REGULAR);
        expectedConcat += oversizedClean;
      } else {
        const char* w = (i % 2 == 0) ? "modern" : "readers";
        para.addWord(w, EpdFontFamily::REGULAR);
        expectedConcat += w;
      }
    }
    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, /*viewportWidth=*/170,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    EXPECT_TRUE(para.isEmpty());
    EXPECT_FALSE(para.hadDroppedWords());
    EXPECT_EQ(reconstructParagraph(lines), expectedConcat);
  }

  // 2. < 400-word paragraph whose oversized word pre-splits and grows words.size() >= 400
  //    falls back to greedy with DISCRETIONARY_HYPHEN_FLAG tokens already present; greedy must
  //    never merge "<prefix>-" with "<suffix>" onto one line.
  {
    const std::string oversized =
        "story\xC2\xAD"
        "telling\xC2\xAD"
        "story\xC2\xAD"
        "telling\xC2\xAD"
        "story\xC2\xAD"
        "telling\xC2\xAD"
        "story\xC2\xAD"
        "telling";
    const std::string oversizedClean = "storytellingstorytellingstorytellingstorytelling";
    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    std::string expectedConcat;
    for (int i = 0; i < 397; ++i) {
      para.addWord("in", EpdFontFamily::REGULAR);
      expectedConcat += "in";
    }
    para.addWord(oversized, EpdFontFamily::REGULAR);
    expectedConcat += oversizedClean;
    para.addWord("end.", EpdFontFamily::REGULAR);
    expectedConcat += "end.";

    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, /*viewportWidth=*/170,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    EXPECT_TRUE(para.isEmpty());
    EXPECT_FALSE(para.hadDroppedWords());
    EXPECT_EQ(reconstructParagraph(lines), expectedConcat);
  }

  // 3. DP bps allocation OOM after oversized-word pre-split routes to greedy and keeps every
  //    DISCRETIONARY_HYPHEN_FLAG prefix strictly line-final.
  {
    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    std::string expectedConcat;
    for (int i = 0; i < 100; ++i) {
      para.addWord("in", EpdFontFamily::REGULAR);
      expectedConcat += "in";
    }
    para.addWord("internationalization", EpdFontFamily::REGULAR);
    expectedConcat += "internationalization";
    para.addWord("end.", EpdFontFamily::REGULAR);
    expectedConcat += "end.";

    gDeniedNothrowArrayAllocs = 0;
    gFailNothrowArrayAllocSize = 400 * 8;  // Deny MAX_DP_BREAKPOINTS BreakPoint[] allocation.
    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, /*viewportWidth=*/170,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    gFailNothrowArrayAllocSize = 0;
    EXPECT_GT(gDeniedNothrowArrayAllocs, 0u);
    EXPECT_TRUE(para.isEmpty());
    EXPECT_EQ(reconstructParagraph(lines), expectedConcat);
  }
}

TEST_F(TypographyLayoutFixture, NonHyphenPreSplitAttachedGroupPreservesZeroWidthAttachment) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  // 1. Apostrophe-contraction trailing word ("international\u00ADly\u00A0all'improvviso"):
  //    pre-split breaks after "all'" with needsHyphen=false, then DP splits "international\u00ADly"
  //    so "all'" and "improvviso" land on the same line with 0 px phantom gap.
  {
    renderer.ensureSdCardFontReady(kFontId, "In a internationally all'improvviso", 0x01);
    const int wWord = renderer.getTextAdvanceX(kFontId, "internationally", EpdFontFamily::REGULAR);
    const int wSp = renderer.getSpaceWidth(kFontId, EpdFontFamily::REGULAR);
    const int wAllApos = renderer.getTextAdvanceX(kFontId, "all'", EpdFontFamily::REGULAR);
    const uint16_t colW = static_cast<uint16_t>(wWord + wSp + wAllApos + 12);

    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    para.addWord("In", EpdFontFamily::REGULAR);
    para.addWord("a", EpdFontFamily::REGULAR);
    para.addWord(
        "international\xC2\xAD"
        "ly",
        EpdFontFamily::REGULAR);
    para.addWord(" ", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true);
    para.addWord("all'improvviso", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true);

    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, colW,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(reconstructParagraph(lines), "Inainternationally all'improvviso");

    const TextBlock& line1 = *lines[1];
    int aposIdx = -1;
    for (uint16_t wi = 0; wi + 1 < line1.wordCount(); ++wi) {
      if (std::string(line1.wordText(wi)) == "all'" && std::string(line1.wordText(wi + 1)) == "improvviso") {
        aposIdx = wi;
        break;
      }
    }
    ASSERT_GE(aposIdx, 0) << "Expected 'all'' and 'improvviso' joined on line 1";
    EXPECT_EQ(line1.wordXpos(aposIdx + 1), line1.wordXpos(aposIdx) + wAllApos);
  }

  // 2. Hangul trailing word ("international\u00ADly\u00A0대한민국프로그래밍"):
  //    pre-split breaks inside Hangul with needsHyphen=false, then DP splits the leading word
  //    so both Hangul halves land on line 1 with 0 px phantom gap.
  {
    renderer.ensureSdCardFontReady(kFontId, "In a internationally 대한민국프로그래밍", 0x01);
    const int wWord = renderer.getTextAdvanceX(kFontId, "internationally", EpdFontFamily::REGULAR);
    const int wSp = renderer.getSpaceWidth(kFontId, EpdFontFamily::REGULAR);
    const int wHanPre = renderer.getTextAdvanceX(kFontId, "대한민", EpdFontFamily::REGULAR);
    const uint16_t colW = static_cast<uint16_t>(wWord + wSp + wHanPre + 6);

    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    para.addWord("In", EpdFontFamily::REGULAR);
    para.addWord("a", EpdFontFamily::REGULAR);
    para.addWord(
        "international\xC2\xAD"
        "ly",
        EpdFontFamily::REGULAR);
    para.addWord(" ", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true);
    para.addWord("대한민국프로그래밍", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true);

    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, colW,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(reconstructParagraph(lines), "Inainternationally 대한민국프로그래밍");

    const TextBlock& line1 = *lines[1];
    int hanIdx = -1;
    for (uint16_t wi = 0; wi + 1 < line1.wordCount(); ++wi) {
      if (std::string(line1.wordText(wi)) == "대한민" && std::string(line1.wordText(wi + 1)) == "국프로그래밍") {
        hanIdx = wi;
        break;
      }
    }
    ASSERT_GE(hanIdx, 0) << "Expected Hangul halves joined on line 1";
    EXPECT_EQ(line1.wordXpos(hanIdx + 1), line1.wordXpos(hanIdx) + wHanPre);
  }
}

TEST_F(TypographyLayoutFixture, LongLooseAndRaggedParagraphsDoNotSaturateDemerits) {
  static constexpr const char* kUpstream[] = {
      "When",       "modern",       "portable",     "readers",    "render",  "classic", "serif",
      "typography", "across",       "narrow",       "columns,",   "careful", "spacing", "between",
      "adjacent",   "entrepreneur", "storytelling", "paragraphs", "0000000",
  };

  auto layoutWords = [&](const CssTextAlign align, const uint16_t colW, const int tailCount) {
    BlockStyle style;
    style.alignment = align;
    style.textIndentDefined = true;
    style.textIndent = 0;
    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : kUpstream) para.addWord(w, EpdFontFamily::REGULAR);
    for (int i = 0; i < tailCount; ++i) para.addWord("1234567", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, colW,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    return lines;
  };

  auto prefixLinesBeforeTail = [](const std::vector<std::unique_ptr<TextBlock>>& lines) {
    std::vector<std::string> out;
    for (const auto& line : lines) {
      if (line->wordCount() > 0 && std::string(line->wordText(0)) == "1234567") break;
      std::string joined;
      for (uint16_t wi = 0; wi < line->wordCount(); ++wi) {
        if (wi > 0) joined += ' ';
        joined += line->wordText(wi);
      }
      out.push_back(std::move(joined));
    }
    return out;
  };

  // 1. Narrow justified paragraph with 6 single-word loose lines at tail must not saturate 2e9
  //    or perturb upstream line breaks compared with a 1-word tail.
  {
    const auto refLines = layoutWords(CssTextAlign::Justify, /*colW=*/200, /*tailCount=*/1);
    const auto tailLines = layoutWords(CssTextAlign::Justify, /*colW=*/200, /*tailCount=*/6);
    ASSERT_GE(tailLines.size(), refLines.size() + 5u);
    EXPECT_EQ(prefixLinesBeforeTail(tailLines), prefixLinesBeforeTail(refLines));
  }

  // 2. 25+ line ragged Left-aligned paragraph must not saturate 2e9 or perturb upstream breaks.
  {
    const auto refLines = layoutWords(CssTextAlign::Left, /*colW=*/240, /*tailCount=*/1);
    const auto tailLines = layoutWords(CssTextAlign::Left, /*colW=*/240, /*tailCount=*/25);
    ASSERT_GE(tailLines.size(), 25u);
    EXPECT_EQ(prefixLinesBeforeTail(tailLines), prefixLinesBeforeTail(refLines));
  }
}

TEST_F(TypographyLayoutFixture, SplitOomOnAttachedNbspGroupPreservesGroupWithoutLoneNbspLine) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  // 1. "Hello 200\u00A0Quadratkilometer": WordStore OOM on DP mid-word split backtracks past
  //    the unbreakable NBSP group so "200 Quadratkilometer" stays intact on line 1.
  {
    Hyphenator::setPreferredLanguage("de");
    renderer.ensureSdCardFontReady(kFontId, "first Hello 200 Quadratkilometer oft.", 0x01);
    const int w200 = renderer.getTextAdvanceX(kFontId, "200", EpdFontFamily::REGULAR);
    const int wSp = renderer.getSpaceWidth(kFontId, EpdFontFamily::REGULAR);
    const int wQuad = renderer.getTextAdvanceX(kFontId, "Quadratkilometer", EpdFontFamily::REGULAR);
    const uint16_t colW = static_cast<uint16_t>(w200 + wSp + wQuad + 40);

    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    // 335 * 6 B = 2010 B used in chunk 0; adding 34 B leaves 4 B (< "Quadrat-" 9 B).
    for (int i = 0; i < 335; ++i) para.addWord("first", EpdFontFamily::REGULAR);
    para.layoutAndExtractLines(renderer, kFontId, 518, [](std::unique_ptr<TextBlock>, uint32_t) {});
    ASSERT_TRUE(para.isEmpty());
    para.resetFirstLineIndent();

    para.addWord("Hello", EpdFontFamily::REGULAR);
    para.addWord("200", EpdFontFamily::REGULAR);
    para.addWord(" ", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true);
    para.addWord("Quadratkilometer", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true);
    para.addWord("oft.", EpdFontFamily::REGULAR);
    ASSERT_FALSE(para.hadDroppedWords());

    gDeniedNothrowArrayAllocs = 0;
    gFailNothrowArrayAllocSize = 2048;
    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, colW,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    gFailNothrowArrayAllocSize = 0;
    Hyphenator::setPreferredLanguage("en");

    EXPECT_GT(gDeniedNothrowArrayAllocs, 0u);
    ASSERT_GE(lines.size(), 2u);
    ASSERT_EQ(lines[0]->wordCount(), 1u);
    EXPECT_STREQ(lines[0]->wordText(0), "Hello");
    ASSERT_GE(lines[1]->wordCount(), 3u);
    EXPECT_STREQ(lines[1]->wordText(0), "200");
    EXPECT_STREQ(lines[1]->wordText(1), " ");
    EXPECT_STREQ(lines[1]->wordText(2), "Quadratkilometer");
  }

  // 2. "In a dim old foo-\u00A0characteristically?": Line 0 ends after "foo-"; line 1 starts
  //    with breakable-attached NBSP + "characteristically" + attached "?". WordStore OOM on
  //    splitting "characteristically" must advance past the attached group rather than emit a
  //    lone-NBSP blank line.
  {
    Hyphenator::setPreferredLanguage("en");
    renderer.ensureSdCardFontReady(kFontId, "first In a dim old foo- characteristically?", 0x01);
    const int wSp = renderer.getSpaceWidth(kFontId, EpdFontFamily::REGULAR);
    const int wChar = renderer.getTextAdvanceX(kFontId, "characteristically", EpdFontFamily::REGULAR);
    const uint16_t colW = static_cast<uint16_t>(wSp + wChar + 2);

    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    // 336 * 6 B = 2016 B used in chunk 0; adding 30 B leaves 2 B (< any prefix split).
    for (int i = 0; i < 336; ++i) para.addWord("first", EpdFontFamily::REGULAR);
    para.layoutAndExtractLines(renderer, kFontId, 518, [](std::unique_ptr<TextBlock>, uint32_t) {});
    ASSERT_TRUE(para.isEmpty());
    para.resetFirstLineIndent();

    para.addWord("In", EpdFontFamily::REGULAR);
    para.addWord("foo-", EpdFontFamily::REGULAR);
    para.addWord(" ", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true);
    para.addWord("characteristically", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true);
    para.addWord("?", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true);
    ASSERT_FALSE(para.hadDroppedWords());

    gDeniedNothrowArrayAllocs = 0;
    gFailNothrowArrayAllocSize = 2048;
    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, colW,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    gFailNothrowArrayAllocSize = 0;

    EXPECT_GT(gDeniedNothrowArrayAllocs, 0u);
    ASSERT_EQ(lines.size(), 2u);
    for (size_t li = 0; li < lines.size(); ++li) {
      ASSERT_GT(lines[li]->wordCount(), 0u);
      EXPECT_FALSE(lines[li]->wordCount() == 1u && std::string(lines[li]->wordText(0)) == " ")
          << "Unexpected lone-NBSP blank line at line " << li;
    }
    EXPECT_STREQ(lines[0]->wordText(lines[0]->wordCount() - 1), "foo-");
    ASSERT_EQ(lines[1]->wordCount(), 2u);
    EXPECT_STREQ(lines[1]->wordText(0), "characteristically");
    EXPECT_STREQ(lines[1]->wordText(1), "?");
  }
}

TEST_F(TypographyLayoutFixture, NonBreakingHyphenCompoundsHyphenateInnerSegmentsWithoutAdjacentSplits) {
  Hyphenator::setPreferredLanguage("en");
  // 1. Normal hyphenation inside U+2011 compound ("super‑international") finds Liang breaks inside
  //    inner alphabetic segments without emitting a break before or after U+2011 (byte offsets 5..8).
  const std::string compound =
      "super\xE2\x80\x91"
      "international";
  const auto normalBreaks = Hyphenator::breakOffsets(compound, /*includeFallback=*/false);
  EXPECT_FALSE(normalBreaks.empty());
  for (const auto& b : normalBreaks) {
    EXPECT_NE(b.byteOffset, 5u);
    EXPECT_NE(b.byteOffset, 8u);
  }

  // 2. Fallback hyphenation on oversized U+2011 compounds and numeric ranges never splits adjacent to U+2011.
  const std::string longCompound =
      "extraordinary\xE2\x80\x91"
      "internationalization";
  for (const auto& b : Hyphenator::breakOffsets(longCompound, /*includeFallback=*/true)) {
    EXPECT_NE(b.byteOffset, 13u);
    EXPECT_NE(b.byteOffset, 16u);
  }
  const std::string numRange =
      "2026\xE2\x80\x91"
      "2027";
  EXPECT_TRUE(Hyphenator::breakOffsets(numRange, /*includeFallback=*/true).empty());
}

TEST_F(TypographyLayoutFixture, Deep80LineTailAnd45OverflowTokensPreserveUpstreamWordPacking) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  renderer.ensureSdCardFontReady(kFontId, "a b c d e internationalization0123456789 end.", 0x01);

  // 1. 80 single-word loose tail lines preceded by short words "a b c d e" pack multiple words on line 0.
  {
    ParsedText para(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : {"a", "b", "c", "d", "e"}) para.addWord(w, EpdFontFamily::REGULAR);
    for (int i = 0; i < 80; ++i) para.addWord("chapters", EpdFontFamily::REGULAR);
    para.addWord("end.", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, /*viewportWidth=*/110,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GT(lines.size(), 50u);
    EXPECT_GE(lines[0]->wordCount(), 4u);
  }

  // 2. 45 forced-overflow unbreakable tokens preceded by short words still pack line 0 normally.
  {
    ParsedText para(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    for (const char* w : {"a", "b", "c", "d", "e"}) para.addWord(w, EpdFontFamily::REGULAR);
    for (int i = 0; i < 45; ++i) para.addWord("01", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, /*viewportWidth=*/140,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines[0]->wordCount(), 5u);
  }
}

TEST_F(TypographyLayoutFixture, CjkQuotedSoftHyphenatedTokenMatchesExtractLineOpticalHang) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  const std::string cjkShy =
      "\xE3\x80\x8C"
      "inter\xC2\xAD"
      "national\xE3\x80\x8D";
  renderer.ensureSdCardFontReady(kFontId, "When clear inter- national observations concluded.", 0x01);
  const int preW = renderer.getTextAdvanceX(kFontId,
                                            "\xE3\x80\x8C"
                                            "inter-",
                                            EpdFontFamily::REGULAR);
  const int baseW = renderer.getTextAdvanceX(kFontId, "When", EpdFontFamily::REGULAR) +
                    renderer.getSpaceAdvance(kFontId, 'n', 'c', EpdFontFamily::REGULAR) +
                    renderer.getTextAdvanceX(kFontId, "clear", EpdFontFamily::REGULAR) + 7;
  const uint16_t colW = static_cast<uint16_t>(baseW + preW - 1);

  ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
  para.addWord("When", EpdFontFamily::REGULAR);
  para.addWord("clear", EpdFontFamily::REGULAR);
  para.addWord(cjkShy, EpdFontFamily::REGULAR);
  para.addWord("observations", EpdFontFamily::REGULAR);
  para.addWord("concluded.", EpdFontFamily::REGULAR);

  std::vector<std::unique_ptr<TextBlock>> lines;
  para.layoutAndExtractLines(renderer, kFontId, colW,
                             [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
  ASSERT_GE(lines.size(), 2u);
  const uint16_t last0 = lines[0]->wordCount() - 1;
  const int rightEdge =
      lines[0]->wordXpos(last0) + renderer.getTextAdvanceX(kFontId, lines[0]->wordText(last0), EpdFontFamily::REGULAR);
  EXPECT_EQ(rightEdge, static_cast<int>(colW));
}
