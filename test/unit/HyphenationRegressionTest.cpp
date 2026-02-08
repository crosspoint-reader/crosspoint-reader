#include <string>
#include <vector>

#include "lib/Epub/Epub/hyphenation/HyphenationCommon.h"
#include "lib/Epub/Epub/hyphenation/Hyphenator.h"
#include "test/test_harness.h"

// These tests specifically target the code paths changed by the hyphenation optimization:
//   1. collectCodepoints() overload that fills a pre-existing vector (static buffer reuse)
//   2. trimSurroundingPunctuationAndFootnote() bulk erase replacing O(n²) erase-from-front
//   3. liangBreakIndexes() static AugmentedWord and scores buffer reuse
//   4. Hyphenator::breakOffsets() static cps buffer reuse

// Helper to compare break offset vectors
void assertBreaksEqual(const std::vector<Hyphenator::BreakInfo>& actual,
                       const std::vector<std::pair<size_t, bool>>& expected, const char* word) {
  if (actual.size() != expected.size()) {
    std::cerr << "  FAIL: \"" << word << "\" — expected " << expected.size() << " breaks, got " << actual.size()
              << "\n";
    ++g_testsFailed;
    return;
  }
  for (size_t i = 0; i < actual.size(); ++i) {
    if (actual[i].byteOffset != expected[i].first || actual[i].requiresInsertedHyphen != expected[i].second) {
      std::cerr << "  FAIL: \"" << word << "\" break[" << i << "] — expected {" << expected[i].first << ", "
                << expected[i].second << "}, got {" << actual[i].byteOffset << ", " << actual[i].requiresInsertedHyphen
                << "}\n";
      ++g_testsFailed;
      return;
    }
  }
  ++g_testsPassed;
}

// --- Golden-value tests for breakOffsets ---
// These values were captured from the algorithm output and verify exact byte offsets.

void testGoldenBreakOffsetsEnglish() {
  Hyphenator::setPreferredLanguage("en");

  // Words with known hyphenation break points
  assertBreaksEqual(Hyphenator::breakOffsets("beautiful", false), {{4, true}, {6, true}}, "beautiful");

  assertBreaksEqual(Hyphenator::breakOffsets("international", false), {{5, true}, {7, true}}, "international");

  assertBreaksEqual(Hyphenator::breakOffsets("communication", false), {{3, true}, {5, true}, {7, true}, {9, true}},
                    "communication");

  assertBreaksEqual(Hyphenator::breakOffsets("responsibility", false), {{6, true}, {8, true}, {11, true}},
                    "responsibility");

  assertBreaksEqual(Hyphenator::breakOffsets("extraordinary", false), {{5, true}, {7, true}, {9, true}},
                    "extraordinary");

  assertBreaksEqual(Hyphenator::breakOffsets("understanding", false), {{5, true}, {10, true}}, "understanding");

  assertBreaksEqual(Hyphenator::breakOffsets("computer", false), {{3, true}}, "computer");

  assertBreaksEqual(Hyphenator::breakOffsets("implementation", false), {{5, true}, {8, true}, {10, true}},
                    "implementation");

  assertBreaksEqual(Hyphenator::breakOffsets("encyclopedia", false), {{4, true}, {7, true}, {9, true}}, "encyclopedia");

  assertBreaksEqual(Hyphenator::breakOffsets("characterization", false),
                    {{4, true}, {6, true}, {9, true}, {10, true}, {12, true}}, "characterization");
}

void testGoldenNoBreaks() {
  Hyphenator::setPreferredLanguage("en");

  // Short words should have no breaks (min prefix=3, min suffix=3 for English)
  assertBreaksEqual(Hyphenator::breakOffsets("hello", false), {}, "hello");
  assertBreaksEqual(Hyphenator::breakOffsets("world", false), {}, "world");
  assertBreaksEqual(Hyphenator::breakOffsets("the", false), {}, "the");
  assertBreaksEqual(Hyphenator::breakOffsets("a", false), {}, "a");
  assertBreaksEqual(Hyphenator::breakOffsets("hi", false), {}, "hi");
  assertBreaksEqual(Hyphenator::breakOffsets("cat", false), {}, "cat");
}

void testEmptyWord() {
  Hyphenator::setPreferredLanguage("en");
  assertBreaksEqual(Hyphenator::breakOffsets("", false), {}, "empty");
}

// --- collectCodepoints overload regression ---
// The new overload fills a pre-existing vector. Verify it matches the original.

void testCollectCodepointsOverloadMatchesOriginal() {
  const std::vector<std::string> words = {"hello",    "café",         "naïve",   "", "a",
                                          "\xC3\xA9", "\xE2\x80\x93", "test123", "Ÿ"};

  for (const auto& word : words) {
    auto original = collectCodepoints(word);

    std::vector<CodepointInfo> filled;
    collectCodepoints(word, filled);

    if (original.size() != filled.size()) {
      std::cerr << "  FAIL: collectCodepoints size mismatch for \"" << word << "\": " << original.size() << " vs "
                << filled.size() << "\n";
      ++g_testsFailed;
      return;
    }

    for (size_t i = 0; i < original.size(); ++i) {
      if (original[i].value != filled[i].value || original[i].byteOffset != filled[i].byteOffset) {
        std::cerr << "  FAIL: collectCodepoints mismatch at index " << i << " for \"" << word << "\"\n";
        ++g_testsFailed;
        return;
      }
    }
  }
  ++g_testsPassed;
}

// --- collectCodepoints buffer reuse ---
// Calling the overload multiple times should not leak data between calls.

void testCollectCodepointsBufferReuse() {
  std::vector<CodepointInfo> buf;

  // First call with a long word
  collectCodepoints("international", buf);
  ASSERT_EQ(buf.size(), static_cast<size_t>(13));

  // Second call with a short word — buffer should be fully replaced
  collectCodepoints("hi", buf);
  ASSERT_EQ(buf.size(), static_cast<size_t>(2));
  ASSERT_EQ(buf[0].value, static_cast<uint32_t>('h'));
  ASSERT_EQ(buf[1].value, static_cast<uint32_t>('i'));

  // Third call with empty — buffer should be empty
  collectCodepoints("", buf);
  ASSERT_EQ(buf.size(), static_cast<size_t>(0));
}

// --- trimSurroundingPunctuationAndFootnote regression ---
// The optimization replaced the O(n²) erase-from-front loop with a bulk erase.

void testTrimLeadingPunctuation() {
  auto cps = collectCodepoints("...hello");
  trimSurroundingPunctuationAndFootnote(cps);
  // Should have removed the three dots, leaving "hello"
  ASSERT_EQ(cps.size(), static_cast<size_t>(5));
  ASSERT_EQ(cps[0].value, static_cast<uint32_t>('h'));
}

void testTrimTrailingPunctuation() {
  auto cps = collectCodepoints("hello...");
  trimSurroundingPunctuationAndFootnote(cps);
  ASSERT_EQ(cps.size(), static_cast<size_t>(5));
  ASSERT_EQ(cps[4].value, static_cast<uint32_t>('o'));
}

void testTrimBothSides() {
  auto cps = collectCodepoints("\"hello!\"");
  trimSurroundingPunctuationAndFootnote(cps);
  ASSERT_EQ(cps.size(), static_cast<size_t>(5));
  ASSERT_EQ(cps[0].value, static_cast<uint32_t>('h'));
  ASSERT_EQ(cps[4].value, static_cast<uint32_t>('o'));
}

void testTrimAllPunctuation() {
  auto cps = collectCodepoints("...,,,!!!");
  trimSurroundingPunctuationAndFootnote(cps);
  ASSERT_EQ(cps.size(), static_cast<size_t>(0));
}

void testTrimNoPunctuation() {
  auto cps = collectCodepoints("hello");
  const size_t originalSize = cps.size();
  trimSurroundingPunctuationAndFootnote(cps);
  ASSERT_EQ(cps.size(), originalSize);
}

void testTrimFootnoteReference() {
  auto cps = collectCodepoints("word[12]");
  trimSurroundingPunctuationAndFootnote(cps);
  // Should remove the [12] footnote reference
  ASSERT_EQ(cps.size(), static_cast<size_t>(4));
  ASSERT_EQ(cps[3].value, static_cast<uint32_t>('d'));
}

void testTrimEmpty() {
  std::vector<CodepointInfo> cps;
  trimSurroundingPunctuationAndFootnote(cps);  // should not crash
  ASSERT_EQ(cps.size(), static_cast<size_t>(0));
}

void testTrimSingleLetter() {
  auto cps = collectCodepoints("a");
  trimSurroundingPunctuationAndFootnote(cps);
  ASSERT_EQ(cps.size(), static_cast<size_t>(1));
  ASSERT_EQ(cps[0].value, static_cast<uint32_t>('a'));
}

void testTrimSinglePunctuation() {
  auto cps = collectCodepoints(".");
  trimSurroundingPunctuationAndFootnote(cps);
  ASSERT_EQ(cps.size(), static_cast<size_t>(0));
}

// --- Static buffer safety: repeated calls with varied inputs ---
// The optimization uses static local vectors in breakOffsets and liangBreakIndexes.
// Verify that results are deterministic and do not leak across calls.

void testStaticBufferSafety() {
  Hyphenator::setPreferredLanguage("en");

  const std::vector<std::string> words = {"beautiful",     "the",          "implementation", "cat",
                                          "extraordinary", "hello",        "computer",       "",
                                          "encyclopedia",  "understanding"};

  // Capture reference results
  std::vector<std::vector<Hyphenator::BreakInfo>> reference;
  for (const auto& w : words) {
    reference.push_back(Hyphenator::breakOffsets(w, false));
  }

  // Repeat 50 times and verify identical results every time
  for (int iter = 0; iter < 50; ++iter) {
    for (size_t i = 0; i < words.size(); ++i) {
      auto result = Hyphenator::breakOffsets(words[i], false);
      if (result.size() != reference[i].size()) {
        std::cerr << "  FAIL: iteration " << iter << ", word \"" << words[i] << "\" — size mismatch: " << result.size()
                  << " vs " << reference[i].size() << "\n";
        ++g_testsFailed;
        return;
      }
      for (size_t j = 0; j < result.size(); ++j) {
        if (result[j].byteOffset != reference[i][j].byteOffset ||
            result[j].requiresInsertedHyphen != reference[i][j].requiresInsertedHyphen) {
          std::cerr << "  FAIL: iteration " << iter << ", word \"" << words[i] << "\" break[" << j << "] mismatch\n";
          ++g_testsFailed;
          return;
        }
      }
    }
  }
  ++g_testsPassed;
}

// --- Interleaved short and long words ---
// Tests that static buffers properly resize when alternating between word lengths.

void testInterleavedWordLengths() {
  Hyphenator::setPreferredLanguage("en");

  // Alternate between very long and very short words
  for (int i = 0; i < 20; ++i) {
    auto longResult = Hyphenator::breakOffsets("characterization", false);
    assertBreaksEqual(longResult, {{4, true}, {6, true}, {9, true}, {10, true}, {12, true}}, "characterization");

    auto shortResult = Hyphenator::breakOffsets("a", false);
    assertBreaksEqual(shortResult, {}, "a");

    auto emptyResult = Hyphenator::breakOffsets("", false);
    assertBreaksEqual(emptyResult, {}, "empty");

    auto medResult = Hyphenator::breakOffsets("computer", false);
    assertBreaksEqual(medResult, {{3, true}}, "computer");
  }
}

// --- Words with explicit hyphens ---

void testExplicitHyphenBreaks() {
  Hyphenator::setPreferredLanguage("en");

  auto result = Hyphenator::breakOffsets("well-known", false);
  // Should detect the explicit hyphen and break after it
  ASSERT_TRUE(result.size() >= 1);
  if (!result.empty()) {
    // The break should be at the character after the hyphen
    ASSERT_EQ(result[0].byteOffset, static_cast<size_t>(5));  // "well-" is 5 bytes, break at "k"
  }
}

void testSoftHyphen() {
  Hyphenator::setPreferredLanguage("en");

  // Soft hyphen (U+00AD) between 'beau' and 'tiful'
  std::string word = "beau\xC2\xADtiful";
  auto result = Hyphenator::breakOffsets(word, false);
  ASSERT_TRUE(result.size() >= 1);
}

int main() {
  std::cout << "HyphenationRegressionTest\n";

  // Golden values
  RUN_TEST(testGoldenBreakOffsetsEnglish);
  RUN_TEST(testGoldenNoBreaks);
  RUN_TEST(testEmptyWord);

  // collectCodepoints overload
  RUN_TEST(testCollectCodepointsOverloadMatchesOriginal);
  RUN_TEST(testCollectCodepointsBufferReuse);

  // trimSurroundingPunctuationAndFootnote
  RUN_TEST(testTrimLeadingPunctuation);
  RUN_TEST(testTrimTrailingPunctuation);
  RUN_TEST(testTrimBothSides);
  RUN_TEST(testTrimAllPunctuation);
  RUN_TEST(testTrimNoPunctuation);
  RUN_TEST(testTrimFootnoteReference);
  RUN_TEST(testTrimEmpty);
  RUN_TEST(testTrimSingleLetter);
  RUN_TEST(testTrimSinglePunctuation);

  // Static buffer safety
  RUN_TEST(testStaticBufferSafety);
  RUN_TEST(testInterleavedWordLengths);

  // Explicit hyphens
  RUN_TEST(testExplicitHyphenBreaks);
  RUN_TEST(testSoftHyphen);

  TEST_SUMMARY();
}
