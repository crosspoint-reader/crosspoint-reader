#include <gtest/gtest.h>

#include <cstdlib>
#include <map>
#include <new>
#include <string>

#include "FontCacheManager.h"
#include "FontDecompressor.h"
#include "SdCardFont.h"
#include "ShapingTokens.h"
#include "Utf8.h"

namespace {

bool countHeapAllocations = false;
size_t heapAllocationCount = 0;

// TTF fonts are PSRAM-gated and stubbed out on the host (see stubs/TtfEpdFont.h);
// every manager gets an empty map for that slot.
const std::map<int, TtfEpdFont*> kNoTtfFonts;

const SdCardFont::PrewarmCall* findCall(const SdCardFont& font, const uint8_t styleMask) {
  for (int i = 0; i < font.prewarmCallCount; i++) {
    if (font.prewarmCalls[i].styleMask == styleMask) return &font.prewarmCalls[i];
  }
  return nullptr;
}

}  // namespace

void* operator new(const size_t size) {
  if (countHeapAllocations) heapAllocationCount++;
  if (void* allocation = std::malloc(size)) return allocation;
  throw std::bad_alloc();
}

void operator delete(void* allocation) noexcept { std::free(allocation); }

void operator delete(void* allocation, size_t) noexcept { std::free(allocation); }

TEST(FontCacheManagerTest, PrewarmScopeBatchesEachFontAndResolvedStyleSeparately) {
  SdCardFont readerFont;
  SdCardFont fallbackFont;
  const std::map<int, EpdFontFamily> noBuiltinFonts;
  const std::map<int, SdCardFont*> sdFonts{{-17, &readerFont}, {23, &fallbackFont}};
  FontCacheManager manager(noBuiltinFonts, sdFonts, kNoTtfFonts);

  auto scope = manager.createPrewarmScope();
  manager.recordText("A", -17, EpdFontFamily::REGULAR);
  manager.recordText("B", -17, EpdFontFamily::BOLD);
  manager.recordText("C", 23, EpdFontFamily::REGULAR);
  scope.endScanAndPrewarm();

  ASSERT_EQ(2, readerFont.prewarmCallCount);
  const auto* regularCall = findCall(readerFont, 0x01);
  ASSERT_NE(nullptr, regularCall);
  EXPECT_STREQ("A", regularCall->text);
  EXPECT_FALSE(regularCall->accumulate);
  const auto* boldCall = findCall(readerFont, 0x02);
  ASSERT_NE(nullptr, boldCall);
  EXPECT_STREQ("B", boldCall->text);
  EXPECT_FALSE(boldCall->accumulate);

  ASSERT_EQ(1, fallbackFont.prewarmCallCount);
  EXPECT_STREQ("C", fallbackFont.prewarmCalls[0].text);
  EXPECT_EQ(0x01, fallbackFont.prewarmCalls[0].styleMask);
}

TEST(FontCacheManagerTest, PrewarmScopeMergesStylesThatResolveToTheSameSdFontData) {
  SdCardFont font;
  font.resolvedStyles[EpdFontFamily::BOLD] = EpdFontFamily::REGULAR;
  const std::map<int, EpdFontFamily> noBuiltinFonts;
  const std::map<int, SdCardFont*> sdFonts{{7, &font}};
  FontCacheManager manager(noBuiltinFonts, sdFonts, kNoTtfFonts);

  auto scope = manager.createPrewarmScope();
  manager.recordText("A", 7, EpdFontFamily::REGULAR);
  manager.recordText("B", 7, EpdFontFamily::BOLD);
  scope.endScanAndPrewarm();

  ASSERT_EQ(1, font.prewarmCallCount);
  EXPECT_STREQ("AB", font.prewarmCalls[0].text);
  EXPECT_EQ(0x01, font.prewarmCalls[0].styleMask);
}

TEST(FontCacheManagerTest, PrewarmScopeMergesBuiltInStylesThatShareFontData) {
  EpdFontData regularData{reinterpret_cast<const void*>(1)};
  EpdFontData boldData{reinterpret_cast<const void*>(1)};
  const EpdFontFamily family(&regularData, &boldData, &regularData, nullptr);
  const std::map<int, EpdFontFamily> builtinFonts{{5, family}};
  const std::map<int, SdCardFont*> noSdFonts;
  FontDecompressor decompressor;
  FontCacheManager manager(builtinFonts, noSdFonts, kNoTtfFonts);
  manager.setFontDecompressor(&decompressor);

  auto scope = manager.createPrewarmScope();
  manager.recordText("A", 5, EpdFontFamily::REGULAR);
  manager.recordText("B", 5, EpdFontFamily::BOLD);
  manager.recordText("C", 5, EpdFontFamily::ITALIC);
  scope.endScanAndPrewarm();

  ASSERT_EQ(2, decompressor.prewarmCallCount);
  const auto& first = decompressor.prewarmCalls[0];
  const auto& second = decompressor.prewarmCalls[1];
  const auto* regularCall = first.fontData == &regularData ? &first : &second;
  const auto* boldCall = first.fontData == &boldData ? &first : &second;
  EXPECT_STREQ("AC", regularCall->text);
  EXPECT_STREQ("B", boldCall->text);
}

TEST(FontCacheManagerTest, PrewarmScopePreservesUniqueMultibyteCodepoints) {
  SdCardFont font;
  const std::map<int, EpdFontFamily> noBuiltinFonts;
  const std::map<int, SdCardFont*> sdFonts{{7, &font}};
  FontCacheManager manager(noBuiltinFonts, sdFonts, kNoTtfFonts);

  auto scope = manager.createPrewarmScope();
  manager.recordText("\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80\xC3\xA9", 7, EpdFontFamily::REGULAR);
  scope.endScanAndPrewarm();

  ASSERT_EQ(1, font.prewarmCallCount);
  EXPECT_STREQ("\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80", font.prewarmCalls[0].text);
}

TEST(FontCacheManagerTest, PrewarmScanDoesNotAllocateHeapMemory) {
  SdCardFont font;
  const std::map<int, EpdFontFamily> noBuiltinFonts;
  const std::map<int, SdCardFont*> sdFonts{{7, &font}};
  FontCacheManager manager(noBuiltinFonts, sdFonts, kNoTtfFonts);

  heapAllocationCount = 0;
  countHeapAllocations = true;
  auto scope = manager.createPrewarmScope();
  manager.recordText("Repeated text: \xC3\xA9 \xE4\xB8\xAD \xF0\x9F\x98\x80", 7, EpdFontFamily::REGULAR);
  scope.endScanAndPrewarm();
  countHeapAllocations = false;

  EXPECT_EQ(0U, heapAllocationCount);
}

TEST(FontCacheManagerTest, IncrementalPrewarmRequestsAccumulation) {
  SdCardFont font;
  const std::map<int, EpdFontFamily> noBuiltinFonts;
  const std::map<int, SdCardFont*> sdFonts{{7, &font}};
  FontCacheManager manager(noBuiltinFonts, sdFonts, kNoTtfFonts);
  manager.prewarmCache(7, "title", 1);
  ASSERT_EQ(1, font.prewarmCallCount);
  EXPECT_TRUE(font.prewarmCalls[0].accumulate);
}

TEST(FontCacheManagerTest, ReleaseCallbackFiresOnReleaseSdFontCaches) {
  const std::map<int, EpdFontFamily> noBuiltinFonts;
  const std::map<int, SdCardFont*> noSdFonts;
  FontCacheManager manager(noBuiltinFonts, noSdFonts, kNoTtfFonts);

  int callbackCallCount = 0;
  manager.setReleaseCallback([](void* ctx) { (*static_cast<int*>(ctx))++; }, &callbackCallCount);

  manager.releaseSdFontCaches();
  EXPECT_EQ(1, callbackCallCount);
}

TEST(FontCacheManagerTest, IdleLookaheadDefaultsToOneForNonTtfFonts) {
  const std::map<int, EpdFontFamily> noBuiltinFonts;
  const std::map<int, SdCardFont*> noSdFonts;
  FontCacheManager manager(noBuiltinFonts, noSdFonts, kNoTtfFonts);

  EXPECT_EQ(1, manager.getIdleLookaheadPages(7));
}

TEST(FontCacheManagerTest, CompactAfterPrewarmForwardsActiveStyleMasksAndOrdersRegularFirst) {
  SdCardFont bodyFont;
  SdCardFont unusedVariantFont;
  EpdFontData uncompressedUiData{nullptr};
  const EpdFontFamily uiFamily(&uncompressedUiData, nullptr, nullptr, nullptr);
  const std::map<int, EpdFontFamily> builtinFonts{{11, uiFamily}};
  const std::map<int, SdCardFont*> sdFonts{{-17, &bodyFont}, {-18, &unusedVariantFont}};
  FontDecompressor decompressor;
  FontCacheManager manager(builtinFonts, sdFonts, kNoTtfFonts);
  manager.setFontDecompressor(&decompressor);

  {
    auto scope = manager.createPrewarmScope();
    // Record Bold before Regular: Phase 2 must still prewarm Regular (0x01) before Bold (0x02).
    manager.recordText("Bold", -17, EpdFontFamily::BOLD);
    manager.recordText("Hello", -17, EpdFontFamily::REGULAR);
    manager.recordText("12/124", 11, EpdFontFamily::REGULAR);
    EXPECT_TRUE(scope.endScanAndPrewarm());
    manager.compactAfterPrewarm(/*dropLayoutAdvanceCache=*/true);
  }

  ASSERT_EQ(2, bodyFont.prewarmCallCount);
  EXPECT_EQ(0x01u, bodyFont.prewarmCalls[0].styleMask);
  EXPECT_EQ(0x02u, bodyFont.prewarmCalls[1].styleMask);

  EXPECT_EQ(1, bodyFont.compactCallCount);
  EXPECT_EQ(0x03u, bodyFont.lastCompactStyleMask);
  EXPECT_TRUE(bodyFont.lastCompactDropAdvance);
  EXPECT_EQ(1, unusedVariantFont.compactCallCount);
  EXPECT_EQ(0x00u, unusedVariantFont.lastCompactStyleMask);

  // A subsequent empty scan resets lastScanFontCount_ so compactAfterPrewarm evicts all SD fonts.
  {
    auto emptyScope = manager.createPrewarmScope();
    EXPECT_TRUE(emptyScope.endScanAndPrewarm());
    manager.compactAfterPrewarm(/*dropLayoutAdvanceCache=*/false);
  }
  EXPECT_EQ(2, bodyFont.compactCallCount);
  EXPECT_EQ(0x00u, bodyFont.lastCompactStyleMask);
  EXPECT_FALSE(bodyFont.lastCompactDropAdvance);
}

TEST(FontCacheManagerTest, DenseScanPastCodepointCapAndFontSlotCapPreservesActiveSdStyles) {
  // Build 512 unique UTF-8 codepoints (U+0100..U+02FF) to saturate MAX_SCAN_CODEPOINTS (512).
  std::string denseRegular;
  denseRegular.reserve(512 * 2);
  for (uint32_t cp = 0x0100; cp < 0x0300; ++cp) {
    utf8AppendCodepoint(cp, denseRegular);
  }

  // Scenario 1: Regular fills all 512 scan codepoints, followed by Bold "AV" on the same SD font.
  // Bold prewarm is skipped due to the codepoint cap, yet compactAfterPrewarm must pass 0x03!
  {
    SdCardFont bodyFont;
    const std::map<int, EpdFontFamily> noBuiltinFonts;
    const std::map<int, SdCardFont*> sdFonts{{-17, &bodyFont}};
    FontCacheManager manager(noBuiltinFonts, sdFonts, kNoTtfFonts);

    heapAllocationCount = 0;
    countHeapAllocations = true;
    auto scope = manager.createPrewarmScope();
    manager.recordText(denseRegular.c_str(), -17, EpdFontFamily::REGULAR);
    manager.recordText("AV", -17, EpdFontFamily::BOLD);
    EXPECT_TRUE(scope.endScanAndPrewarm());
    manager.compactAfterPrewarm(/*dropLayoutAdvanceCache=*/true);
    countHeapAllocations = false;

    EXPECT_EQ(0U, heapAllocationCount);
    ASSERT_EQ(1, bodyFont.prewarmCallCount);
    EXPECT_EQ(0x01u, bodyFont.prewarmCalls[0].styleMask);
    EXPECT_EQ(1, bodyFont.compactCallCount);
    EXPECT_EQ(0x03u, bodyFont.lastCompactStyleMask);
  }

  // Scenario 2: 4 earlier fonts saturate MAX_SCAN_FONTS (4); a 5th SD font scanned in Bold + Italic
  // preserves its exact 0x06 style mask in compactAfterPrewarm while a 6th unreferenced SD font gets 0x00.
  {
    EpdFontData uiData{nullptr};
    const EpdFontFamily uiFamily(&uiData, nullptr, nullptr, nullptr);
    const std::map<int, EpdFontFamily> builtinFonts{{1, uiFamily}, {2, uiFamily}, {3, uiFamily}};
    SdCardFont fourthFont;
    SdCardFont fifthFont;
    SdCardFont unusedSixthFont;
    const std::map<int, SdCardFont*> sdFonts{{-24, &fourthFont}, {-25, &fifthFont}, {-26, &unusedSixthFont}};
    FontCacheManager manager(builtinFonts, sdFonts, kNoTtfFonts);

    {
      auto scope = manager.createPrewarmScope();
      manager.recordText("a", 1, EpdFontFamily::REGULAR);
      manager.recordText("b", 2, EpdFontFamily::REGULAR);
      manager.recordText("c", 3, EpdFontFamily::REGULAR);
      manager.recordText("d", -24, EpdFontFamily::REGULAR);
      manager.recordText("Bold", -25, EpdFontFamily::BOLD);
      manager.recordText("Ital", -25, EpdFontFamily::ITALIC);
      EXPECT_TRUE(scope.endScanAndPrewarm());
      manager.compactAfterPrewarm(/*dropLayoutAdvanceCache=*/true);
    }

    EXPECT_EQ(1, fourthFont.prewarmCallCount);
    EXPECT_EQ(0x01u, fourthFont.lastCompactStyleMask);
    EXPECT_EQ(0, fifthFont.prewarmCallCount);
    EXPECT_EQ(0x06u, fifthFont.lastCompactStyleMask);
    EXPECT_EQ(0x00u, unusedSixthFont.lastCompactStyleMask);

    // Next render scope resets fifthFont's scannedStyleMask_ when it is no longer drawn.
    {
      auto nextScope = manager.createPrewarmScope();
      manager.recordText("d", -24, EpdFontFamily::REGULAR);
      EXPECT_TRUE(nextScope.endScanAndPrewarm());
      manager.compactAfterPrewarm(/*dropLayoutAdvanceCache=*/false);
    }
    EXPECT_EQ(0x01u, fourthFont.lastCompactStyleMask);
    EXPECT_EQ(0x00u, fifthFont.lastCompactStyleMask);
  }
}

TEST(FontCacheManagerTest, ShapedTokensSkipPositionTokensAndPreserveScannedStyleMask) {
  SdCardFont indicFont;
  const std::map<int, EpdFontFamily> noBuiltinFonts;
  const std::map<int, SdCardFont*> sdFonts{{-42, &indicFont}};
  FontCacheManager manager(noBuiltinFonts, sdFonts, kNoTtfFonts);

  std::string shapedWord;
  utf8AppendCodepoint(shaping::advanceToken(128), shapedWord);
  utf8AppendCodepoint(shaping::offsetToken(1, -2), shapedWord);
  utf8AppendCodepoint(shaping::glyphToken(42), shapedWord);

  std::string expectedGlyphOnly;
  utf8AppendCodepoint(shaping::glyphToken(42), expectedGlyphOnly);

  {
    auto scope = manager.createPrewarmScope();
    manager.recordText(shapedWord.c_str(), -42, EpdFontFamily::BOLD);
    EXPECT_TRUE(scope.endScanAndPrewarm());
    manager.compactAfterPrewarm(/*dropLayoutAdvanceCache=*/true);
  }

  ASSERT_EQ(1, indicFont.prewarmCallCount);
  EXPECT_EQ(0x02u, indicFont.prewarmCalls[0].styleMask);
  EXPECT_STREQ(expectedGlyphOnly.c_str(), indicFont.prewarmCalls[0].text);
  EXPECT_EQ(0x02u, indicFont.lastCompactStyleMask);
}
