#include <gtest/gtest.h>

#include <cstring>

#include "activities/reader/EpubLinkBackStack.h"

namespace {

using Entry = EpubLinkBackStack::Entry;
constexpr int kMax = EpubLinkBackStack::kMaxDepth;
constexpr int kSpines = 2000;

Entry entry(const int spine, const int page, const int pageCount = 0, const uint16_t paragraph = 0) {
  Entry e;
  e.spineIndex = spine;
  e.pageNumber = page;
  e.pageCount = pageCount;
  e.paragraphIndex = paragraph;
  e.hasParagraph = paragraph != 0;
  return e;
}

TEST(EpubLinkBackStack, AFullStackRoundTrips) {
  // Above 255 in every field: both bytes of each matter. The middle entry has no paragraph and no
  // page count, as one recorded mid-build on a chapter of wrapped paragraphs is stored.
  Entry stack[kMax] = {entry(1733, 300, 412, 1025), entry(5, 12), entry(1999, 65535, 700, 4096)};
  uint8_t data[EpubLinkBackStack::kMaxSize];
  const size_t size = EpubLinkBackStack::encode(stack, kMax, data);
  ASSERT_EQ(EpubLinkBackStack::kMaxSize, size);

  Entry back[kMax];
  ASSERT_EQ(kMax, EpubLinkBackStack::decode(data, size, kSpines, back));
  for (int i = 0; i < kMax; i++) {
    EXPECT_EQ(stack[i].spineIndex, back[i].spineIndex) << i;
    EXPECT_EQ(stack[i].pageNumber, back[i].pageNumber) << i;
    EXPECT_EQ(stack[i].pageCount, back[i].pageCount) << i;
    EXPECT_EQ(stack[i].hasParagraph, back[i].hasParagraph) << i;
    EXPECT_EQ(stack[i].paragraphIndex, back[i].paragraphIndex) << i;
  }
}

TEST(EpubLinkBackStack, AShallowStackStoresOnlyItsEntries) {
  Entry stack[kMax] = {entry(7, 3, 20, 9)};
  uint8_t data[EpubLinkBackStack::kMaxSize];
  const size_t size = EpubLinkBackStack::encode(stack, 1, data);
  EXPECT_EQ(2 + EpubLinkBackStack::kEntrySize, size);

  Entry back[kMax];
  ASSERT_EQ(1, EpubLinkBackStack::decode(data, size, kSpines, back));
  EXPECT_EQ(7, back[0].spineIndex);
  EXPECT_EQ(9, back[0].paragraphIndex);
}

TEST(EpubLinkBackStack, AnEmptyStackIsNoFile) {
  Entry stack[kMax] = {};
  uint8_t data[EpubLinkBackStack::kMaxSize];
  EXPECT_EQ(0u, EpubLinkBackStack::encode(stack, 0, data));
}

TEST(EpubLinkBackStack, AParagraphNotMarkedValidIsNotStored) {
  Entry stack[kMax] = {entry(1, 2)};
  stack[0].paragraphIndex = 77;  // stale value behind hasParagraph == false
  uint8_t data[EpubLinkBackStack::kMaxSize];
  const size_t size = EpubLinkBackStack::encode(stack, 1, data);

  Entry back[kMax];
  ASSERT_EQ(1, EpubLinkBackStack::decode(data, size, kSpines, back));
  EXPECT_FALSE(back[0].hasParagraph);
  EXPECT_EQ(0, back[0].paragraphIndex) << "0 is the paragraph LUT's none";
}

TEST(EpubLinkBackStack, AFullStackDropsItsOldestEntry) {
  Entry stack[kMax] = {};
  int depth = 0;
  int session = 0;
  for (int jump = 1; jump <= kMax + 1; jump++) EpubLinkBackStack::push(stack, depth, session, entry(jump, jump * 10));

  ASSERT_EQ(kMax, depth);
  // Back from the newest jump returns to where it was made; the first origin is the one given up.
  EXPECT_EQ(kMax + 1, stack[depth - 1].spineIndex);
  for (int i = 0; i < kMax; i++) EXPECT_EQ(i + 2, stack[i].spineIndex) << i;
  EXPECT_EQ(kMax, session) << "every entry left is from this session";
}

TEST(EpubLinkBackStack, PushBelowCapacityAppends) {
  Entry stack[kMax] = {};
  int depth = 0;
  int session = 0;
  EpubLinkBackStack::push(stack, depth, session, entry(4, 1));
  EpubLinkBackStack::push(stack, depth, session, entry(9, 2));
  ASSERT_EQ(2, depth);
  EXPECT_EQ(2, session);
  EXPECT_EQ(4, stack[0].spineIndex);
  EXPECT_EQ(9, stack[1].spineIndex);
}

// A stack loaded from linkstack.bin is for Back only: the KOSync push on sleep is skipped while a
// link followed THIS session is still open, as it was before the stack outlived the session.
TEST(EpubLinkBackStack, ALoadedStackIsNotALinkFollowedThisSession) {
  Entry stack[kMax] = {entry(1, 1), entry(2, 2)};
  int depth = 2;  // as decode() leaves it
  int session = 0;

  EpubLinkBackStack::push(stack, depth, session, entry(3, 3));
  EXPECT_EQ(3, depth);
  EXPECT_EQ(1, session);

  EpubLinkBackStack::pop(depth, session);  // back from the link followed this session
  EXPECT_EQ(2, depth);
  EXPECT_EQ(0, session) << "the sleep push is due again";

  EpubLinkBackStack::pop(depth, session);  // back from a loaded entry
  EXPECT_EQ(1, depth);
  EXPECT_EQ(0, session) << "never below zero";
}

TEST(EpubLinkBackStack, SessionPushesOverAFullLoadedStackCountOnlyWhatIsLeft) {
  Entry stack[kMax] = {entry(1, 1), entry(2, 2), entry(3, 3)};
  int depth = kMax;
  int session = 0;

  EpubLinkBackStack::push(stack, depth, session, entry(10, 0));  // drops loaded entry 1
  EXPECT_EQ(1, session);
  for (int jump = 11; jump <= 14; jump++) EpubLinkBackStack::push(stack, depth, session, entry(jump, 0));
  EXPECT_EQ(kMax, depth);
  EXPECT_EQ(kMax, session) << "the loaded entries have all been dropped";

  for (int i = 0; i < kMax; i++) EpubLinkBackStack::pop(depth, session);
  EXPECT_EQ(0, depth);
  EXPECT_EQ(0, session);
}

// The reader on chapter 3 page 10 follows a note into the notes chapter (spine 7), then the note's
// "Return" link back to its marker. The return lands on the page it was followed from, so the
// stack unwinds to empty, as two presses of Back would have: Back no longer goes to the note, and
// the session has no link open any more.
TEST(EpubLinkBackStack, AReturnLinkUnwindsToBelowItsOrigin) {
  Entry stack[kMax] = {};
  int depth = 0;
  int session = 0;
  EpubLinkBackStack::push(stack, depth, session, entry(3, 10));  // the note marker, tapped
  EpubLinkBackStack::push(stack, depth, session, entry(7, 2));   // the note's Return, tapped

  const int target = EpubLinkBackStack::returnDepth(stack, depth, 3, 10);
  ASSERT_EQ(0, target);
  while (depth > target) EpubLinkBackStack::pop(depth, session);
  EXPECT_EQ(0, depth);
  EXPECT_EQ(0, session) << "the sleep push is due again";
}

// The marker's id names the page of a nearby line, so the return can land a page either side.
TEST(EpubLinkBackStack, AReturnMayLandAPageEitherSideOfItsOrigin) {
  const Entry stack[kMax] = {entry(3, 10), entry(7, 2)};
  EXPECT_EQ(0, EpubLinkBackStack::returnDepth(stack, 2, 3, 9));
  EXPECT_EQ(0, EpubLinkBackStack::returnDepth(stack, 2, 3, 11));
  EXPECT_EQ(-1, EpubLinkBackStack::returnDepth(stack, 2, 3, 8));
  EXPECT_EQ(-1, EpubLinkBackStack::returnDepth(stack, 2, 3, 12));
  EXPECT_EQ(-1, EpubLinkBackStack::returnDepth(stack, 2, 4, 10)) << "another spine is a new detour";
}

// The newest entry was pushed for the link being checked, so landing near it is no return: a link
// to the next page ("see the figure below") is a detour like any other.
TEST(EpubLinkBackStack, TheEntryPushedForTheLinkItselfIsNeverMatched) {
  const Entry stack[kMax] = {entry(3, 10), entry(3, 40)};
  EXPECT_EQ(-1, EpubLinkBackStack::returnDepth(stack, 1, 3, 11));
  EXPECT_EQ(-1, EpubLinkBackStack::returnDepth(stack, 2, 3, 40));
  EXPECT_EQ(-1, EpubLinkBackStack::returnDepth(stack, 0, 3, 10));
}

// Two detours deep, the return from the inner note unwinds only that one: the newest match wins,
// and Back then returns to where the first link was followed from.
TEST(EpubLinkBackStack, TheNewestMatchingOriginIsTheOneReturnedTo) {
  const Entry stack[kMax] = {entry(3, 10), entry(3, 11), entry(7, 2)};
  EXPECT_EQ(1, EpubLinkBackStack::returnDepth(stack, 3, 3, 11));
  EXPECT_EQ(1, EpubLinkBackStack::returnDepth(stack, 3, 3, 10)) << "within a page of both";
  EXPECT_EQ(0, EpubLinkBackStack::returnDepth(stack, 3, 3, 9));
}

TEST(EpubLinkBackStack, PopOnAnEmptyStackDoesNothing) {
  int depth = 0;
  int session = 0;
  EpubLinkBackStack::pop(depth, session);
  EXPECT_EQ(0, depth);
  EXPECT_EQ(0, session);
}

class EpubLinkBackStackRejects : public ::testing::Test {
 protected:
  void SetUp() override {
    Entry stack[kMax] = {entry(1, 2, 3, 4), entry(5, 6, 7, 8)};
    uint8_t encoded[EpubLinkBackStack::kMaxSize];
    size = EpubLinkBackStack::encode(stack, 2, encoded);
    std::memcpy(data, encoded, size);
    for (Entry& e : out) e = entry(42, 42);
  }

  // A rejected file must leave the reader's stack exactly as it was.
  void expectRejected(const uint8_t* bytes, const size_t n, const int spineCount = kSpines) {
    EXPECT_EQ(0, EpubLinkBackStack::decode(bytes, n, spineCount, out));
    for (const Entry& e : out) EXPECT_EQ(42, e.spineIndex);
  }

  uint8_t data[EpubLinkBackStack::kMaxSize + 1] = {};  // one spare byte, for the over-long file
  size_t size = 0;
  Entry out[kMax];
};

TEST_F(EpubLinkBackStackRejects, ATornFile) {
  expectRejected(data, 0);
  expectRejected(data, 1);
  expectRejected(data, size - 1);
}

TEST_F(EpubLinkBackStackRejects, AFileLongerThanItsDepth) { expectRejected(data, size + 1); }

TEST_F(EpubLinkBackStackRejects, AnotherVersion) {
  data[0] = EpubLinkBackStack::kVersion + 1;
  expectRejected(data, size);
}

TEST_F(EpubLinkBackStackRejects, ADepthOutOfRange) {
  data[1] = 0;
  expectRejected(data, 2);
  data[1] = kMax + 1;
  expectRejected(data, 2 + (kMax + 1) * EpubLinkBackStack::kEntrySize);
}

TEST_F(EpubLinkBackStackRejects, AnEntryForASpineTheBookNoLongerHas) {
  // The second entry names spine 5: a book of five spines (0..4) cannot take the stack, and the
  // first entry, valid on its own, is not kept without it.
  expectRejected(data, size, 5);
  EXPECT_EQ(2, EpubLinkBackStack::decode(data, size, 6, out));
}

// The upstream format (crosspoint-reader links.bin: depth, then spine and page per entry) must
// not read as a stack if it ever lands under this name.
TEST_F(EpubLinkBackStackRejects, UpstreamsLayout) {
  const uint8_t upstream[] = {1, 3, 0, 12, 0};
  expectRejected(upstream, sizeof(upstream));
}

}  // namespace
