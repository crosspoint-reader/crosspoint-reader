#include <Epub/ReaderRenderSpec.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "ClippingStore.h"
#include "clippings/ClippingPreview.h"
#include "clippings/ClippingText.h"
#include "clippings/SelectionGeometry.h"

namespace {
struct Reader {
  std::string text;
  size_t pos = 0;
  size_t maxRequest = 0;
  int read(char* out, size_t size) {
    maxRequest = std::max(maxRequest, size);
    const size_t count = std::min(size, text.size() - pos);
    std::memcpy(out, text.data() + pos, count);
    pos += count;
    return static_cast<int>(count);
  }
};
struct PreviewOutput {
  std::array<char, clippingPreview::BUFFER_BYTES> buffer{};
  size_t length = 0;

  std::string text() const { return std::string(buffer.data(), length); }
};

bool readPreview(Reader& reader, const size_t remaining, PreviewOutput& out) {
  return clippingPreview::read(reader, remaining, out.buffer.data(), out.buffer.size(), out.length);
}
constexpr const char* ELLIPSIS = "\xe2\x80\xa6";
}  // namespace

TEST(ClippingPreview, ShortTextNormalizesWithoutEllipsis) {
  Reader reader{" \t\r\nhello\r\nworld\t "};
  PreviewOutput out;
  ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
  EXPECT_EQ(out.text(), "hello world");
}

TEST(ClippingPreview, LongTextUsesBoundedReadsAndMarksOmission) {
  Reader reader{std::string(4096, 'x')};
  PreviewOutput out;
  ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
  EXPECT_EQ(out.text(), std::string(256, 'x') + ELLIPSIS);
  EXPECT_LE(reader.pos, 320u);
  EXPECT_LE(reader.maxRequest, 64u);
}

TEST(ClippingPreview, ExactLimitDoesNotClaimOmission) {
  Reader reader{std::string(256, 'x')};
  PreviewOutput out;
  ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
  EXPECT_EQ(out.text(), reader.text);
}

TEST(ClippingPreview, Utf8CodepointsAreNeverSplitAtLimit) {
  for (const auto& codepoint : {std::string("é"), std::string("中"), std::string("😀")}) {
    for (size_t space = 0; space < codepoint.size(); ++space) {
      const std::string prefix(256 - space, 'a');
      Reader reader{prefix + codepoint + "tail"};
      PreviewOutput out;
      ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
      EXPECT_EQ(out.text(), prefix + ELLIPSIS);
    }
  }
}

TEST(ClippingPreview, Utf8CrossesReadChunkBoundary) {
  Reader reader{std::string(63, 'a') + "😀" + "é中"};
  PreviewOutput out;
  ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
  EXPECT_EQ(out.text(), reader.text);
}

TEST(ClippingPreview, LeadingWhitespaceDoesNotConsumePreviewBudget) {
  Reader reader{std::string(600, ' ') + "\xc2\xa0\xe2\x80\x83\xe2\x80\xaf" + std::string(300, 'x')};
  PreviewOutput out;
  ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
  EXPECT_EQ(out.text(), std::string(256, 'x') + ELLIPSIS);
  EXPECT_LE(reader.maxRequest, 64u);
}

TEST(ClippingPreview, UnicodeWhitespaceCollapsesAndTrailingWhitespaceIsDropped) {
  Reader reader{"hello\xc2\xa0\xe2\x80\x83\xe2\x80\xafworld \t"};
  PreviewOutput out;
  ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
  EXPECT_EQ(out.text(), "hello world");
}

TEST(ClippingPreview, AllWhitespaceAndEmptyTextStayEmpty) {
  for (const auto& text : {std::string(), std::string(4096, ' ')}) {
    Reader reader{text};
    PreviewOutput out;
    std::memcpy(out.buffer.data(), "stale", 6);
    out.length = 5;
    ASSERT_TRUE(readPreview(reader, text.size(), out));
    EXPECT_EQ(out.length, 0u);
    EXPECT_EQ(out.buffer[0], '\0');
    EXPECT_EQ(reader.pos, text.size());
  }
}

TEST(ClippingPreview, DoesNotReadFollowingRecord) {
  Reader reader{"oneNEXT_RECORD"};
  PreviewOutput out;
  ASSERT_TRUE(readPreview(reader, 3, out));
  EXPECT_EQ(out.text(), "one");
  EXPECT_EQ(reader.pos, 3u);
}

TEST(ClippingPreview, ShortReadClearsOldAndPartialOutput) {
  Reader reader{std::string(70, 'a')};
  PreviewOutput out;
  std::memcpy(out.buffer.data(), "stale", 6);
  out.length = 5;
  EXPECT_FALSE(readPreview(reader, 128, out));
  EXPECT_EQ(out.length, 0u);
  EXPECT_EQ(out.buffer[0], '\0');
}

TEST(ClippingPreview, InvalidOrIncompleteUtf8FailsWithoutPartialOutput) {
  for (const auto& text : {std::string("a\xff"), std::string("a\xc2"), std::string("a\xc2x")}) {
    Reader reader{text};
    PreviewOutput out;
    EXPECT_FALSE(readPreview(reader, text.size(), out));
    EXPECT_EQ(out.length, 0u);
    EXPECT_EQ(out.buffer[0], '\0');
  }
}

TEST(ClippingPreview, RepeatedReadsReuseFixedStorage) {
  Reader reader{std::string(4096, 'x')};
  PreviewOutput out;
  const char* buffer = out.buffer.data();
  for (int i = 0; i < 3; ++i) {
    reader.pos = 0;
    ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
    EXPECT_EQ(out.buffer.data(), buffer);
  }
}

TEST(ClippingPreview, InternalWhitespaceStillCountsAgainstReadBudget) {
  Reader reader{"hello" + std::string(4000, ' ') + "world"};
  PreviewOutput out;
  ASSERT_TRUE(readPreview(reader, reader.text.size(), out));
  EXPECT_EQ(out.text(), std::string("hello") + ELLIPSIS);
  EXPECT_LE(reader.pos, 320u);
}

TEST(SelectionGeometry, SinglePressSelectsTwoWordsInEitherReadingDirection) {
  for (const bool rtl : {false, true}) {
    // Reading-order indices run left-to-right in LTR and right-to-left in RTL.
    const int x[] = {rtl ? 300 : 100, 200, rtl ? 100 : 300};
    const int anchor = 1;
    int selected = selectionGeometry::horizontalIndex(anchor, 3, false, true, rtl);
    EXPECT_GT(x[selected], x[anchor]);
    EXPECT_EQ(std::abs(selected - anchor) + 1, 2);
    for (int frame = 0; frame < 20; ++frame) {
      // A held key, delayed refresh, and release have no new press edge.
      selected = selectionGeometry::horizontalIndex(selected, 3, false, false, rtl);
      EXPECT_EQ(std::abs(selected - anchor) + 1, 2);
    }
    EXPECT_EQ(selectionGeometry::horizontalIndex(selected, 3, false, true, rtl), selected);
    selected = selectionGeometry::horizontalIndex(selected, 3, true, false, rtl);
    EXPECT_EQ(selected, anchor);
    selected = selectionGeometry::horizontalIndex(selected, 3, true, false, rtl);
    EXPECT_LT(x[selected], x[anchor]);
    EXPECT_EQ(selectionGeometry::horizontalIndex(selected, 3, true, false, rtl), selected);
    EXPECT_EQ(selectionGeometry::horizontalIndex(selected, 3, true, true, rtl), selected);
  }
  EXPECT_EQ(selectionGeometry::horizontalIndex(0, 0, false, true), -1);
}

TEST(SelectionGeometry, ToolbarOverlaysAtTopAndStaysInsideEveryOrientedSafeArea) {
  for (const Rect safe :
       {Rect{12, 20, 456, 744}, Rect{20, 12, 744, 456}, Rect{8, 32, 456, 744}, Rect{32, 8, 744, 456}}) {
    for (const int top : {safe.y, safe.y + 40, safe.y + safe.height / 2, safe.y + safe.height - 80}) {
      const Rect actions = selectionGeometry::actions(safe, top, 64, 10);
      EXPECT_GE(actions.y, safe.y);
      EXPECT_LE(actions.x + actions.width, safe.x + safe.width);
      EXPECT_LE(actions.y + actions.height, safe.y + safe.height);
      if (top >= safe.y + 74)
        EXPECT_LE(actions.y + actions.height, top - 10);
      else
        EXPECT_EQ(actions.y, safe.y);
      for (int i = 0; i < 3; ++i) {
        const Rect button = selectionGeometry::button(actions, i, 8);
        EXPECT_EQ(selectionGeometry::actionAt(actions, 8, button.x, button.y), i);
        EXPECT_EQ(selectionGeometry::actionAt(actions, 8, button.x + button.width - 1, button.y + button.height - 1),
                  i);
        EXPECT_EQ(selectionGeometry::actionAt(actions, 8, button.x + button.width, button.y), -1);
      }
      EXPECT_EQ(selectionGeometry::actionAt(actions, 8, actions.x, actions.y), -1);
      EXPECT_EQ(selectionGeometry::actionAt(actions, 8, actions.x + actions.width, actions.y), -1);
      EXPECT_EQ(selectionGeometry::actionAt(actions, 8, actions.x, actions.y + actions.height), -1);
    }
  }
}

TEST(SelectionGeometry, FirstButtonFollowsStartHandleUntilScreenEdge) {
  const Rect safe{12, 20, 456, 744};
  const Rect row = selectionGeometry::actions(safe, 200, 48, 10, 260, 92);
  EXPECT_EQ(selectionGeometry::button(row, 0, 8).x, 100);
  const Rect atRight = selectionGeometry::actions(safe, 200, 48, 10, 260, 420);
  EXPECT_EQ(atRight.x + atRight.width, safe.x + safe.width);
  const Rect atLeft = selectionGeometry::actions(safe, 200, 48, 10, 260, 0);
  EXPECT_EQ(atLeft.x, safe.x);
}

TEST(SelectionGeometry, VerticalDragFindsTheNextLineAcrossWhitespace) {
  const Rect first{20, 100, 80, 20};
  const Rect nextLeft{20, 140, 20, 20};
  const Rect nextRight{90, 140, 20, 20};
  const Rect shortLastLine{20, 180, 20, 20};
  // The same x lands in a gap on the next line and beyond the short last line.
  EXPECT_TRUE(selectionGeometry::nearerWord(nextLeft, first, 50, 150));
  EXPECT_FALSE(selectionGeometry::nearerWord(nextRight, nextLeft, 50, 150));
  EXPECT_TRUE(selectionGeometry::nearerWord(shortLastLine, nextLeft, 50, 190));
  EXPECT_TRUE(selectionGeometry::nearerWord(first, nextLeft, 50, 110));
  // Keep a long word under the finger even when a neighbour's centre is closer.
  EXPECT_FALSE(selectionGeometry::nearerWord(Rect{110, 100, 10, 20}, first, 99, 110));
}

TEST(SelectionGeometry, PageAdvanceAcceptsBottomEdgeAcrossThePageWidth) {
  for (const Rect safe : {Rect{0, 0, 480, 750}, Rect{40, 0, 760, 480}, Rect{0, 40, 480, 760}}) {
    for (const int x : {safe.x, safe.x + safe.width / 2, safe.x + safe.width - 1}) {
      EXPECT_TRUE(selectionGeometry::atBottomEdge(safe, 24, x, safe.y + safe.height - 1));
      EXPECT_TRUE(selectionGeometry::atBottomEdge(safe, 24, x, safe.y + safe.height + 5));
      EXPECT_FALSE(selectionGeometry::atBottomEdge(safe, 24, x, safe.y + safe.height - 25));
    }
    EXPECT_FALSE(selectionGeometry::atBottomEdge(safe, 24, safe.x - 1, safe.y + safe.height));
  }
}

TEST(ClippingHighlight, RemovalMatchesPortableAndLegacyHighlightBoundaries) {
  Clipping clip;
  clip.spineIndex = 2;
  clip.startOffset = 100;
  clip.endOffset = 200;
  EXPECT_TRUE(clippingContainsWord(clip, 2, 99, 300, 42, 0, 95, 105));
  EXPECT_TRUE(clippingContainsWord(clip, 2, 0, 1, 0, 0, 190, 210));
  EXPECT_FALSE(clippingContainsWord(clip, 3, 0, 1, 0, 0, 100, 110));
  EXPECT_FALSE(clippingContainsWord(clip, 2, 0, 1, 0, 0, 90, 100));
  EXPECT_FALSE(clippingContainsWord(clip, 2, 0, 1, 0, 0, 200, 210));
  EXPECT_FALSE(clippingContainsWord(clip, 2, 0, 1, 0, 0, UINT32_MAX, UINT32_MAX));
  clip.startOffset = clip.endOffset = UINT32_MAX;
  clip.startPage = 2;
  clip.endPage = 3;
  clip.startWordIndex = 4;
  clip.endWordIndex = 6;
  clip.pageCount = 10;
  clip.layoutSignature = 42;
  EXPECT_TRUE(clippingContainsWord(clip, 2, 2, 10, 42, 4, 0, 1));
  EXPECT_TRUE(clippingContainsWord(clip, 2, 3, 10, 42, 6, 0, 1));
  EXPECT_FALSE(clippingContainsWord(clip, 2, 2, 10, 42, 3, 0, 1));
  EXPECT_FALSE(clippingContainsWord(clip, 2, 3, 10, 42, 7, 0, 1));
  EXPECT_FALSE(clippingContainsWord(clip, 2, 2, 10, 43, 4, 0, 1));
  EXPECT_FALSE(clippingContainsWord(clip, 2, 2, 11, 42, 4, 0, 1));
}

TEST(ClippingText, PreservesLiteralHyphensAndJoinsLayoutSplits) {
  std::string text;
  ASSERT_TRUE(clippingText::append(text, "well-", 0, 5, 0, 4096));
  ASSERT_TRUE(clippingText::append(text, "known", 5, 10, 0, 4096));
  EXPECT_EQ(text, "well-known");
  text.clear();
  ASSERT_TRUE(clippingText::append(text, "discre-", 0, 6, 0, 4096));
  ASSERT_TRUE(clippingText::append(text, "tionary", 6, 13, 0, 4096));
  EXPECT_EQ(text, "discretionary");
  text.clear();
  ASSERT_TRUE(clippingText::append(text, "가나", 0, 2, 0, 4096));
  ASSERT_TRUE(clippingText::append(text, "다라", 2, 4, 0, 4096));
  EXPECT_EQ(text, "가나다라");
  ASSERT_TRUE(clippingText::append(text, "<tag>", 5, 10, ' ', 4096));
  EXPECT_EQ(text, "가나다라 <tag>");
  const auto before = text;
  EXPECT_FALSE(clippingText::append(text, "too long", 11, 19, ' ', text.size() + 1));
  EXPECT_EQ(text, before);
}

TEST(ClippingHighlight, LegacySignatureIncludesCharacterAndWordSpacing) {
  ReaderRenderSpec spec;
  const auto original = readerRenderSpecSignature(spec);
  spec.characterSpacing = 1;
  EXPECT_NE(readerRenderSpecSignature(spec), original);
  spec.characterSpacing = 0;
  spec.wordSpacingPercent = 110;
  EXPECT_NE(readerRenderSpecSignature(spec), original);
}

TEST(SelectionGeometry, KeepsCursorClearOfHintsOnEitherAxis) {
  EXPECT_EQ(selectionGeometry::keepVisible(0, 20, 40, 700), 40);
  EXPECT_EQ(selectionGeometry::keepVisible(735, 20, 40, 700), -15);
  EXPECT_EQ(selectionGeometry::keepVisible(100, 20, 40, 700), 0);
}
