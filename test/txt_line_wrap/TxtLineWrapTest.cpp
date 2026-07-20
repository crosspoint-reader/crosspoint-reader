#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>

#include "TxtLineWrap.h"
#include "Utf8.h"

namespace {

std::string encode(const std::initializer_list<uint32_t> codepoints) {
  std::string text;
  for (const uint32_t cp : codepoints) {
    utf8AppendCodepoint(cp, text);
  }
  return text;
}

std::string repeatCodepoint(const uint32_t codepoint, const size_t count) {
  std::string text;
  text.reserve(count * 3);
  for (size_t i = 0; i < count; ++i) {
    utf8AppendCodepoint(codepoint, text);
  }
  return text;
}

int codepointCount(const char* text) {
  int width = 0;
  auto* cursor = reinterpret_cast<const unsigned char*>(text);
  while (*cursor) {
    utf8NextCodepoint(&cursor);
    ++width;
  }
  return width;
}

template <typename Measure>
size_t findLegacyLineBreak(std::string& text, const int maxWidth, Measure measure) {
  size_t breakPos = text.size();
  while (breakPos > 0 && measure(text.substr(0, breakPos).c_str()) > maxWidth) {
    const size_t spacePos = text.rfind(' ', breakPos - 1);
    if (spacePos != std::string::npos && spacePos > 0) {
      breakPos = spacePos;
      continue;
    }

    --breakPos;
    while (breakPos > 0 && TxtLineWrap::isContinuationByte(text[breakPos])) {
      --breakPos;
    }
  }
  return breakPos;
}

template <typename Measure>
size_t findLargestFittingPrefixBackward(std::string& text, const int maxWidth, Measure measure) {
  size_t breakPos = text.size();
  while (breakPos > 0) {
    const char saved = text[breakPos];
    text[breakPos] = '\0';
    const int width = measure(text.c_str());
    text[breakPos] = saved;
    if (width <= maxWidth) {
      return breakPos;
    }

    --breakPos;
    while (breakPos > 0 && TxtLineWrap::isContinuationByte(text[breakPos])) {
      --breakPos;
    }
  }
  return 0;
}

}  // namespace

TEST(TxtLineWrapEligibility, AcceptsOnlySimpleMonotonicLtrText) {
  EXPECT_TRUE(TxtLineWrap::isMonotonicLtrText(encode({0x4E2D, 0x6587, 'A', '1', 0xFF0C})));
  EXPECT_TRUE(TxtLineWrap::isMonotonicLtrText("ASCII words and spaces"));
  EXPECT_TRUE(TxtLineWrap::isMonotonicLtrText(encode({0x4E2D, ' ', 0x6587})));

  EXPECT_FALSE(TxtLineWrap::isMonotonicLtrText(""));
  EXPECT_FALSE(TxtLineWrap::isMonotonicLtrText(encode({0x4E2D, 0x0628})));
  EXPECT_FALSE(TxtLineWrap::isMonotonicLtrText(encode({0x4E2D, 0x1F642})));
  EXPECT_FALSE(TxtLineWrap::isMonotonicLtrText(std::string("\xE4", 1)));

  std::string embeddedNul = encode({0x4E2D});
  embeddedNul.push_back('\0');
  embeddedNul += encode({0x6587});
  EXPECT_FALSE(TxtLineWrap::isMonotonicLtrText(embeddedNul));
}

TEST(TxtLineWrapSearch, PreservesLegacyWordBreakAcrossWidths) {
  const std::array<std::string, 4> texts = {"alpha beta gamma delta", "averylongfirstword then short words", "one two",
                                            encode({0x4E2D, ' ', 0x6587, 'A'})};
  const auto measure = [](const char* prefix) {
    int width = 0;
    auto* cursor = reinterpret_cast<const unsigned char*>(prefix);
    while (*cursor) {
      const uint32_t cp = utf8NextCodepoint(&cursor);
      width += cp == 'i' || cp == ' ' ? 1 : (utf8IsCjkBreakable(cp) ? 3 : 2);
    }
    return width;
  };

  for (const std::string& original : texts) {
    for (int maxWidth = 1; maxWidth < measure(original.c_str()); ++maxWidth) {
      std::string binaryText = original;
      std::string legacyText = original;
      const size_t largest = TxtLineWrap::findLargestFittingPrefix(binaryText, maxWidth, measure);
      EXPECT_EQ(TxtLineWrap::preserveWordBreak(binaryText, largest),
                findLegacyLineBreak(legacyText, maxWidth, measure));
    }
  }
}

TEST(TxtLineWrapSearch, FindsLargestWeightedPrefixAndRestoresInput) {
  std::string text = encode({0x7532, 0x4E59, 0x4E19, 0x4E01});
  const std::string original = text;
  int calls = 0;
  const auto measure = [&](const char* prefix) {
    ++calls;
    int width = 0;
    auto* cursor = reinterpret_cast<const unsigned char*>(prefix);
    while (*cursor) {
      switch (utf8NextCodepoint(&cursor)) {
        case 0x7532:
          width += 2;
          break;
        case 0x4E59:
          width += 4;
          break;
        case 0x4E19:
          width += 1;
          break;
        default:
          width += 5;
      }
    }
    return width;
  };

  EXPECT_EQ(TxtLineWrap::findLargestFittingPrefix(text, 7, measure), 9u);
  EXPECT_LE(calls, 3);
  EXPECT_EQ(text, original);
}

TEST(TxtLineWrapSearch, HandlesFourByteBoundariesAndNoFit) {
  std::string text = encode({0x20000, 0x20001, 0x20002});
  EXPECT_EQ(TxtLineWrap::findLargestFittingPrefix(text, 2, codepointCount), 8u);
  EXPECT_EQ(TxtLineWrap::findLargestFittingPrefix(text, 0, codepointCount), 0u);

  std::string mixedWidthText = encode({0x4E2D, 'A', 0x20000});
  EXPECT_EQ(TxtLineWrap::findLargestFittingPrefix(mixedWidthText, 2, codepointCount), 4u);
}

TEST(TxtLineWrapSearch, MatchesBackwardSearchAcrossWidthsAndZeroAdvances) {
  const std::string original = encode({0x7532, 0x4E59, 0x4E19, 0x4E01, 0x4E02});
  const auto measure = [](const char* prefix) {
    int width = 0;
    auto* cursor = reinterpret_cast<const unsigned char*>(prefix);
    while (*cursor) {
      switch (utf8NextCodepoint(&cursor)) {
        case 0x7532:
          width += 2;
          break;
        case 0x4E59:
        case 0x4E01:
          break;
        case 0x4E19:
          width += 3;
          break;
        default:
          width += 4;
      }
    }
    return width;
  };

  for (int maxWidth = 2; maxWidth < measure(original.c_str()); ++maxWidth) {
    std::string binaryText = original;
    std::string backwardText = original;
    EXPECT_EQ(TxtLineWrap::findLargestFittingPrefix(binaryText, maxWidth, measure),
              findLargestFittingPrefixBackward(backwardText, maxWidth, measure));
    EXPECT_EQ(binaryText, original);
  }

  std::string zeroAdvanceText = original;
  EXPECT_EQ(TxtLineWrap::findLargestFittingPrefix(zeroAdvanceText, 2, measure), 6u);
}

TEST(TxtLineWrapSearch, MatchesBackwardSearchWithFarFewerMeasurements) {
  constexpr size_t kIssueCharacterCount = 2136;
  std::string binaryText = repeatCodepoint(0x4E2D, kIssueCharacterCount);
  std::string backwardText = binaryText;
  int binaryCalls = 0;
  int backwardCalls = 0;

  const size_t binaryResult = TxtLineWrap::findLargestFittingPrefix(binaryText, 50, [&](const char* prefix) {
    ++binaryCalls;
    return codepointCount(prefix);
  });
  const size_t backwardResult = findLargestFittingPrefixBackward(backwardText, 50, [&](const char* prefix) {
    ++backwardCalls;
    return codepointCount(prefix);
  });

  EXPECT_EQ(binaryResult, backwardResult);
  EXPECT_EQ(binaryResult, 50u * 3u);
  EXPECT_LE(binaryCalls, 14);
  EXPECT_GT(backwardCalls, 2000);
  EXPECT_EQ(binaryText, backwardText);
}
