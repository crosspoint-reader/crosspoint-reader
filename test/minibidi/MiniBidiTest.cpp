#include <gtest/gtest.h>

extern "C" {
#include "minibidi.h"
}

namespace {

void initLine(const ucschar* codepoints, const int count, bidi_char* line) {
  for (int i = 0; i < count; i++) {
    line[i].origwc = codepoints[i];
    line[i].wc = codepoints[i];
    line[i].index = static_cast<uint16_t>(i);
  }
}

bool containsArabicPresentationForm(const bidi_char* line, const int count) {
  for (int i = 0; i < count; i++) {
    if (line[i].wc >= 0xFE70 && line[i].wc <= 0xFEFC) return true;
  }
  return false;
}

}  // namespace

TEST(MiniBidiArabic, ClassifiesArabicLettersAndDigits) {
  EXPECT_EQ(bidi_class(0x0633), AL);
  EXPECT_EQ(bidi_class(0x064E), NSM);
  EXPECT_EQ(bidi_class(0x0661), AN);
  EXPECT_EQ(bidi_class(0x06F1), EN);
}

TEST(MiniBidiArabic, ShapesArabicWordIntoPresentationForms) {
  constexpr ucschar word[] = {0x0633, 0x0644, 0x0627, 0x0645};
  bidi_char line[4];
  bidi_char shaped[4];
  initLine(word, 4, line);

  ASSERT_EQ(do_shape(line, shaped, 4), 1);
  do_bidi(/*autodir=*/true, /*paragraphLevel=*/0, shaped, 4);

  EXPECT_TRUE(containsArabicPresentationForm(shaped, 4));
  EXPECT_NE(shaped[0].wc, word[0]);
}

TEST(MiniBidiArabic, CollapsesLamAlefLigature) {
  constexpr ucschar word[] = {0x0644, 0x0627};
  bidi_char line[2];
  bidi_char shaped[2];
  initLine(word, 2, line);

  ASSERT_EQ(do_shape(line, shaped, 2), 1);
  do_bidi(/*autodir=*/true, /*paragraphLevel=*/0, shaped, 2);

  bool foundLigature = false;
  bool foundConsumedSlot = false;
  for (const auto& ch : shaped) {
    if (ch.wc == 0xFEFB || ch.wc == 0xFEFC) foundLigature = true;
    if (ch.wc == 0) foundConsumedSlot = true;
  }

  EXPECT_TRUE(foundLigature);
  EXPECT_TRUE(foundConsumedSlot);
}

TEST(MiniBidiArabic, DoesNotCollapseAlefLamSequence) {
  constexpr ucschar word[] = {0x0627, 0x0644};
  bidi_char line[2];
  bidi_char shaped[2];
  initLine(word, 2, line);

  ASSERT_EQ(do_shape(line, shaped, 2), 1);
  do_bidi(/*autodir=*/true, /*paragraphLevel=*/0, shaped, 2);

  for (const auto& ch : shaped) {
    EXPECT_NE(ch.wc, 0xFEFB);
    EXPECT_NE(ch.wc, 0xFEFC);
  }
}