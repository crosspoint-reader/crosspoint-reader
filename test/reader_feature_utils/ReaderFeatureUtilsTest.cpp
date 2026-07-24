#include <gtest/gtest.h>

#include <string>

#include "BookSearchUtils.h"
#include "DictionaryQuery.h"
#include "PowerButtonGesture.h"
#include "VietnameseTelex.h"

namespace {
std::string typeTelex(const std::string& keys, const size_t maxBytes = 0) {
  std::string text;
  size_t cursor = 0;
  for (const char key : keys) {
    EXPECT_TRUE(VietnameseTelex::applyKey(text, cursor, key, maxBytes));
  }
  return text;
}
}  // namespace

TEST(PowerButtonGesture, SingleIsImmediateWhenDoubleClickIsDisabled) {
  PowerButtonGesture gesture;
  EXPECT_EQ(gesture.update(10, true, false, true, 0, false), PowerButtonGesture::Event::None);
  EXPECT_EQ(gesture.update(100, false, true, false, 90, false), PowerButtonGesture::Event::Single);
}

TEST(PowerButtonGesture, ResolvesDoubleClickAtTheWindowBoundary) {
  PowerButtonGesture gesture;
  EXPECT_EQ(gesture.update(100, false, true, false, 50, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(gesture.update(449, true, false, true, 0, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(gesture.update(460, false, true, false, 11, true), PowerButtonGesture::Event::Double);

  PowerButtonGesture boundary;
  EXPECT_EQ(boundary.update(100, false, true, false, 50, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(boundary.update(450, true, false, true, 0, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(boundary.update(451, false, true, false, 1, true), PowerButtonGesture::Event::Double);
}

TEST(PowerButtonGesture, DeliversDelayedSingleAndCancelsPendingClickOnHold) {
  PowerButtonGesture gesture;
  EXPECT_EQ(gesture.update(100, false, true, false, 50, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(gesture.update(449, false, false, false, 50, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(gesture.update(450, false, false, false, 50, true), PowerButtonGesture::Event::Single);

  EXPECT_EQ(gesture.update(1000, true, false, true, 0, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(gesture.update(1499, false, false, true, 499, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(gesture.update(1500, false, false, true, 500, true), PowerButtonGesture::Event::Hold);
  EXPECT_EQ(gesture.update(1510, false, true, false, 510, true), PowerButtonGesture::Event::None);
}

TEST(PowerButtonGesture, CancelAndMillisWrapDoNotLeakClicks) {
  PowerButtonGesture gesture;
  EXPECT_EQ(gesture.update(UINT32_MAX - 100, false, true, false, 10, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(gesture.update(249, false, false, false, 10, true), PowerButtonGesture::Event::Single);

  EXPECT_EQ(gesture.update(1000, false, true, false, 10, true), PowerButtonGesture::Event::None);
  gesture.cancel();
  EXPECT_EQ(gesture.update(2000, false, false, false, 0, true), PowerButtonGesture::Event::None);
}

TEST(DictionaryQuery, BuildsVietnamesePhraseAndComposesNfc) {
  const char* words[] = {"C\xC3\xB4ng", "ngh\xE1\xBB\x87", "th\xC3\xB4ng", "tin"};
  std::string query;
  ASSERT_TRUE(DictionaryQuery::buildPhrase(words, 4, query));
  EXPECT_EQ(query, "c\xC3\xB4ng ngh\xE1\xBB\x87 th\xC3\xB4ng tin");

  const std::string nfd = std::string("xa\xCC\x83");
  const char* nfdWords[] = {nfd.c_str(), "h\xE1\xBB\x99i"};
  ASSERT_TRUE(DictionaryQuery::buildPhrase(nfdWords, 2, query));
  EXPECT_EQ(query, "x\xC3\xA3 h\xE1\xBB\x99i");

  const char* uppercaseWords[] = {"\xC4\x90\xE1\xBB\x9CI", "S\xE1\xBB\x90NG"};
  ASSERT_TRUE(DictionaryQuery::buildPhrase(uppercaseWords, 2, query));
  EXPECT_EQ(query, "\xC4\x91\xE1\xBB\x9Di s\xE1\xBB\x91ng");
}

TEST(DictionaryQuery, TrimsPunctuationAndEnforcesBounds) {
  const char* words[] = {"(x\xC3\xA3", "h\xE1\xBB\x99i),"};
  std::string query;
  ASSERT_TRUE(DictionaryQuery::buildPhrase(words, 2, query));
  EXPECT_EQ(query, "x\xC3\xA3 h\xE1\xBB\x99i");

  const std::string tooLong(256, 'a');
  const char* longWord[] = {tooLong.c_str()};
  EXPECT_FALSE(DictionaryQuery::buildPhrase(longWord, 1, query));
  EXPECT_TRUE(query.empty());

  const char* five[] = {"a", "b", "c", "d", "e"};
  EXPECT_FALSE(DictionaryQuery::buildPhrase(five, 5, query));

  const char* quoted[] = {"“Xã”,", "hội—"};
  ASSERT_TRUE(DictionaryQuery::buildPhrase(quoted, 2, query));
  EXPECT_EQ(query, "xã hội");
}

TEST(BookSearch, MatchesVietnameseWithoutDiacriticsAndRanksExactFirst) {
  const BookSearchQuery titleQuery = makeBookSearchQuery("mat biec");
  EXPECT_EQ(matchBookSearch(titleQuery, "M\xE1\xBA\xAF" "t bi\xE1\xBA\xBF" "c.epub"),
            BookSearchMatch::Folded);
  EXPECT_EQ(matchBookSearch(titleQuery, "Mat biec - ban dep.epub"), BookSearchMatch::Exact);

  const BookSearchQuery authorQuery = makeBookSearchQuery("nguyen nhat anh");
  EXPECT_EQ(matchBookSearch(authorQuery, "Nguy\xE1\xBB\x85n Nh\xE1\xBA\xADt \xC3\x81nh"),
            BookSearchMatch::Folded);
  EXPECT_EQ(matchBookSearch(makeBookSearchQuery("dac nhan tam"),
                            "\xC4\x90\xE1\xBA\xAF" "c nh\xC3\xA2n t\xC3\xA2m"),
            BookSearchMatch::Folded);
}

TEST(BookSearch, HandlesNfdSeparatorsAndBounds) {
  const std::string nfd = std::string("Ma\xCC\x86\xCC\x81" "t_bie\xCC\x82\xCC\x81" "c");
  EXPECT_EQ(matchBookSearch(makeBookSearchQuery("mat biec"), nfd), BookSearchMatch::Folded);
  EXPECT_TRUE(makeBookSearchQuery(std::string(100, 'a')).exact.size() <= BOOK_SEARCH_QUERY_BYTES);
  EXPECT_EQ(matchBookSearch(makeBookSearchQuery("missing"), "M\xE1\xBA\xAF" "t bi\xE1\xBA\xBF" "c"),
            BookSearchMatch::None);
}

TEST(BookSearch, KeepsPageCapacityBestResultsAndReportsOverflow) {
  constexpr size_t pageCapacity = 9;
  std::vector<size_t> results;
  size_t exactCount = 0;
  bool truncated = false;
  EXPECT_TRUE(results.empty());

  for (size_t i = 0; i < pageCapacity; ++i) {
    addRankedBookSearchResult(results, exactCount, truncated, i, BookSearchMatch::Folded, pageCapacity);
    if (i == 0) {
      EXPECT_EQ(results.size(), 1u);
    }
  }
  EXPECT_EQ(results.size(), pageCapacity);
  EXPECT_FALSE(truncated);

  addRankedBookSearchResult(results, exactCount, truncated, pageCapacity, BookSearchMatch::Exact, pageCapacity);
  ASSERT_EQ(results.size(), pageCapacity);
  EXPECT_TRUE(truncated);
  EXPECT_EQ(results.front(), pageCapacity);
  EXPECT_EQ(exactCount, 1u);

  for (size_t i = pageCapacity + 1; i < 40; ++i) {
    addRankedBookSearchResult(results, exactCount, truncated, i, BookSearchMatch::Exact, pageCapacity);
  }
  EXPECT_EQ(results.size(), pageCapacity);
  EXPECT_EQ(exactCount, pageCapacity);
  EXPECT_EQ(results.front(), pageCapacity);
  EXPECT_EQ(results.back(), pageCapacity * 2 - 1);
}

TEST(BookSearch, HonorsZeroOneSevenEightAndHardLimitCapacities) {
  for (const size_t capacity : {size_t{0}, size_t{1}, size_t{7}, size_t{8}, BOOK_SEARCH_RESULT_HARD_LIMIT}) {
    std::vector<size_t> results;
    size_t exactCount = 0;
    bool truncated = false;
    for (size_t i = 0; i <= capacity; ++i) {
      addRankedBookSearchResult(results, exactCount, truncated, i, BookSearchMatch::Folded, capacity);
    }
    EXPECT_EQ(results.size(), capacity);
    EXPECT_TRUE(truncated);
  }
}

TEST(BookSearch, IgnoresNonMatchesWithoutMarkingOverflow) {
  std::vector<size_t> results;
  size_t exactCount = 0;
  bool truncated = false;
  addRankedBookSearchResult(results, exactCount, truncated, 0, BookSearchMatch::None, 0);
  EXPECT_TRUE(results.empty());
  EXPECT_EQ(exactCount, 0u);
  EXPECT_FALSE(truncated);
}

TEST(VietnameseTelex, ComposesVietnameseVowelsAndTonesAsNfc) {
  EXPECT_EQ(typeTelex("aas"), "ấ");
  EXPECT_EQ(typeTelex("awj"), "ặ");
  EXPECT_EQ(typeTelex("oox"), "ỗ");
  EXPECT_EQ(typeTelex("owr"), "ở");
  EXPECT_EQ(typeTelex("uwj"), "ự");
  EXPECT_EQ(typeTelex("dd"), "đ");
  EXPECT_EQ(typeTelex("w"), "ư");
  EXPECT_EQ(typeTelex("uow"), "ươ");
}

TEST(VietnameseTelex, TypesCommonVietnameseWordsAndPhrases) {
  EXPECT_EQ(typeTelex("tieengs Vieetj"), "tiếng Việt");
  EXPECT_EQ(typeTelex("dduwowngf"), "đường");
  EXPECT_EQ(typeTelex("hoangf"), "hoàng");
  EXPECT_EQ(typeTelex("hoaf"), "hòa");
  EXPECT_EQ(typeTelex("khoer"), "khỏe");
  EXPECT_EQ(typeTelex("thuyr"), "thủy");
  EXPECT_EQ(typeTelex("quas"), "quá");
  EXPECT_EQ(typeTelex("giaf"), "già");
  EXPECT_EQ(typeTelex("ngoaif"), "ngoài");
}

TEST(VietnameseTelex, RepositionsAndReplacesToneMarks) {
  EXPECT_EQ(typeTelex("toans"), "toán");
  EXPECT_EQ(typeTelex("toasn"), "toán");
  EXPECT_EQ(typeTelex("toanfs"), "toán");

  std::string afterDelete = typeTelex("toans");
  afterDelete.pop_back();
  size_t cursor = afterDelete.size();
  ASSERT_TRUE(VietnameseTelex::normalizeAtCursor(afterDelete, cursor));
  EXPECT_EQ(afterDelete, "tóa");
}

TEST(VietnameseTelex, PreservesCaseAndSupportsEscapeKeys) {
  EXPECT_EQ(typeTelex("AAS"), "Ấ");
  EXPECT_EQ(typeTelex("Dd"), "Đ");
  EXPECT_EQ(typeTelex("Tieengs"), "Tiếng");
  EXPECT_EQ(typeTelex("aaa"), "aa");
  EXPECT_EQ(typeTelex("aww"), "aw");
  EXPECT_EQ(typeTelex("ddd"), "dd");
  EXPECT_EQ(typeTelex("herr"), "her");
  EXPECT_EQ(typeTelex("az"), "az");
  EXPECT_EQ(typeTelex("toansz"), "toan");
}

TEST(VietnameseTelex, DoesNotShapeVowelsSeparatedByConsonants) {
  EXPECT_EQ(typeTelex("banana"), "banana");
  EXPECT_EQ(typeTelex("camera"), "camera");
  EXPECT_EQ(typeTelex("internet"), "internet");
  EXPECT_EQ(typeTelex("facebook"), "facebook");
  EXPECT_EQ(typeTelex("kaka"), "kaka");
}

TEST(VietnameseTelex, HandlesCursorEditsAndByteLimitsAtomically) {
  std::string text = "tn";
  size_t cursor = 1;
  for (const char key : std::string("oas")) {
    ASSERT_TRUE(VietnameseTelex::applyKey(text, cursor, key));
  }
  EXPECT_EQ(text, "toán");
  EXPECT_EQ(cursor, std::string("toá").size());

  text = "a";
  cursor = 1;
  EXPECT_FALSE(VietnameseTelex::applyKey(text, cursor, 'w', 1));
  EXPECT_EQ(text, "a");
  EXPECT_EQ(cursor, 1u);

  text = "á";
  cursor = 1;
  EXPECT_FALSE(VietnameseTelex::applyKey(text, cursor, 'n'));
  EXPECT_EQ(text, "á");
  EXPECT_EQ(cursor, 1u);
}

TEST(VietnameseTelex, BoundsWorkToTheCurrentToken) {
  std::string text(17, 'b');
  size_t cursor = text.size();
  ASSERT_TRUE(VietnameseTelex::applyKey(text, cursor, 's'));
  EXPECT_EQ(text, std::string(17, 'b') + "s");
  EXPECT_EQ(cursor, 18u);
}
