// The reader's link back-stack (activities/reader/LinkBackStack.h).
#include <gtest/gtest.h>

#include "activities/reader/LinkBackStack.h"

namespace {

using link_stack::Position;

constexpr int kCapacity = 3;  // EpubReaderActivity::MAX_FOOTNOTE_DEPTH

// Mirrors how the reader drives the helpers: a followed link pushes the page it was tapped on,
// its landing is applied once the page shows, Back pops one entry, an explicit jump clears.
struct Stack {
  Position entries[kCapacity] = {};
  int depth = 0;
  Position at = {0, 0};

  void follow(const Position& landed) {
    link_stack::push(entries, depth, kCapacity, at);
    depth = link_stack::depthAfterLanding(entries, depth, landed);
    at = landed;
  }
  void back() {
    ASSERT_GT(depth, 0);
    at = entries[--depth];
  }
  void explicitJump(const Position& to) {
    depth = 0;
    at = to;
  }
};

bool eq(const Position& a, const Position& b) { return link_stack::samePage(a, b); }

constexpr Position kText = {3, 12};
constexpr Position kNote = {9, 4};

TEST(LinkBackStack, PushRecordsTheOriginAsNewest) {
  Position entries[kCapacity] = {};
  int depth = 0;
  link_stack::push(entries, depth, kCapacity, {1, 2});
  link_stack::push(entries, depth, kCapacity, {3, 4});
  EXPECT_EQ(depth, 2);
  EXPECT_TRUE(eq(entries[0], {1, 2}));
  EXPECT_TRUE(eq(entries[1], {3, 4}));
}

TEST(LinkBackStack, FullStackDropsTheOldest) {
  Position entries[kCapacity] = {};
  int depth = 0;
  for (int i = 1; i <= 5; i++) link_stack::push(entries, depth, kCapacity, {i, i * 10});
  EXPECT_EQ(depth, kCapacity);
  EXPECT_TRUE(eq(entries[0], {3, 30}));
  EXPECT_TRUE(eq(entries[1], {4, 40}));
  EXPECT_TRUE(eq(entries[2], {5, 50}));
}

TEST(LinkBackStack, BackLinkToTheOriginPopsBothEntries) {
  Stack s;
  s.at = kText;
  s.follow(kNote);  // noteref -> note
  EXPECT_EQ(s.depth, 1);
  s.follow(kText);  // the note's own back-arrow
  EXPECT_EQ(s.depth, 0);
}

TEST(LinkBackStack, LandingElsewhereKeepsTheStack) {
  Stack s;
  s.at = kText;
  s.follow(kNote);
  s.follow({3, 13});  // next page of the text, not the one the note was opened from
  EXPECT_EQ(s.depth, 2);
  s.follow({7, 0});
  EXPECT_EQ(s.depth, 3);
  // Same page number, other spine: not a return.
  Stack t;
  t.at = kText;
  t.follow(kNote);
  t.follow({4, kText.pageNumber});
  EXPECT_EQ(t.depth, 2);
}

TEST(LinkBackStack, ShallowStacksPopOnlyOnAMatch) {
  Position entries[kCapacity] = {};
  // Depth 0: nothing to return to.
  EXPECT_EQ(link_stack::depthAfterLanding(entries, 0, {0, 0}), 0);
  entries[0] = kText;
  // Depth 1, landing elsewhere: an ordinary jump.
  EXPECT_EQ(link_stack::depthAfterLanding(entries, 1, kNote), 1);
  // Depth 1, landing on its own origin (a same-page link): Back would only have stayed put.
  EXPECT_EQ(link_stack::depthAfterLanding(entries, 1, kText), 0);
}

TEST(LinkBackStack, ChainedNoteReturnToTheRootPopsEverything) {
  // text -> note1 -> note2, and note2's back-arrow points at the noteref in the text.
  const Position note1 = {9, 4};
  const Position note2 = {9, 20};
  Stack s;
  s.at = kText;
  s.follow(note1);  // [text]
  s.follow(note2);  // [text, note1]
  s.follow(kText);  // push note2 -> [text, note1, note2]; text == entries[0]
  EXPECT_EQ(s.depth, 0);
}

TEST(LinkBackStack, ReturnCutsTheStackToTheMatchedOrigin) {
  const Position e0 = {1, 0}, e1 = {2, 0}, e2 = {3, 0};
  const Position entries[kCapacity] = {e0, e1, e2};
  // Landing on entries[i] is Back pressed (depth - i) times: depth i.
  EXPECT_EQ(link_stack::depthAfterLanding(entries, 3, e1), 1);
  EXPECT_EQ(link_stack::depthAfterLanding(entries, 3, e0), 0);
  // entries[depth-1] is the jump just made: landing on its page drops just that entry.
  EXPECT_EQ(link_stack::depthAfterLanding(entries, 3, e2), 2);
  EXPECT_EQ(link_stack::depthAfterLanding(entries, 2, e1), 1);
  // Entries at or above depth are not part of the stack.
  EXPECT_EQ(link_stack::depthAfterLanding(entries, 1, e1), 1);
  EXPECT_EQ(link_stack::depthAfterLanding(entries, 2, e2), 2);
}

TEST(LinkBackStack, DuplicateOriginsPopToTheNewestMatch) {
  const Position x = {1, 5}, y = {2, 0}, z = {3, 0};
  // The page appears twice below the newest entry: the newer copy wins, as repeated Back would
  // stop there first.
  const Position twice[kCapacity] = {x, x, z};
  EXPECT_EQ(link_stack::depthAfterLanding(twice, 3, x), 1);
  // The newest copy is the entry just pushed: only it goes.
  const Position split[kCapacity] = {x, y, x};
  EXPECT_EQ(link_stack::depthAfterLanding(split, 3, x), 2);

  // Reached by reading: paging back to the text and following another link from it records the
  // same origin twice.
  Stack s;
  s.at = x;
  s.follow(y);  // [x]
  s.at = x;     // page turns back to x: no landing, no stack change
  s.follow(z);  // [x, x]
  s.follow(x);  // push z -> [x, x, z]; newest x is entries[1]
  EXPECT_EQ(s.depth, 1);
  EXPECT_TRUE(eq(s.entries[0], x));
}

TEST(LinkBackStack, LinkToItsOwnTapPagePopsJustThatEntry) {
  Stack s;
  s.at = kText;
  s.follow(kNote);   // [text]
  s.follow({9, 7});  // [text, note]
  s.follow({9, 7});  // push p7 -> [text, note, note p7]; lands on its own origin
  EXPECT_EQ(s.depth, 2);
  EXPECT_TRUE(eq(s.entries[1], kNote));
}

TEST(LinkBackStack, NestedNotesUnwindOneReturnAtATime) {
  const Position noteA = {9, 4};
  const Position noteB = {9, 20};
  Stack s;
  s.at = kText;
  s.follow(noteA);  // [text]
  s.follow(noteB);  // [text, A]
  EXPECT_EQ(s.depth, 2);
  s.follow(noteA);  // B's back-arrow -> A: [text]
  EXPECT_EQ(s.depth, 1);
  EXPECT_TRUE(eq(s.entries[0], kText));
  s.follow(kText);  // A's back-arrow -> text
  EXPECT_EQ(s.depth, 0);
}

TEST(LinkBackStack, BackButtonAfterAReturnHasNothingStale) {
  Stack s;
  s.at = kText;
  s.follow(kNote);
  s.follow(kText);
  EXPECT_EQ(s.depth, 0);  // Back now falls through to the reader's ordinary Back
  // Back still works for an unreturned note.
  s.follow(kNote);
  s.back();
  EXPECT_EQ(s.depth, 0);
  EXPECT_TRUE(eq(s.at, kText));
}

TEST(LinkBackStack, ReturnAcrossTheDropOldestBoundary) {
  // Full stack, then a back-link: the push drops entries[0], so the comparison must look at the
  // shifted array (the origin of the previous jump now sits at entries[capacity - 2]).
  const Position p0 = {1, 0}, p1 = {2, 0}, p2 = {3, 0}, p3 = {4, 0};
  Stack s;
  s.at = p0;
  s.follow(p1);  // [p0]
  s.follow(p2);  // [p0, p1]
  s.follow(p3);  // [p0, p1, p2]
  EXPECT_EQ(s.depth, kCapacity);
  s.follow(p2);  // p3's back-link to p2: push p3 drops p0 -> [p1, p2, p3]; p2 == entries[1]
  EXPECT_EQ(s.depth, 1);
  EXPECT_TRUE(eq(s.entries[0], p1));
  s.back();
  EXPECT_TRUE(eq(s.at, p1));
  EXPECT_EQ(s.depth, 0);
}

TEST(LinkBackStack, FullStackLandingOnTheDroppedEntryIsNotAReturn) {
  const Position p0 = {1, 0}, p1 = {2, 0}, p2 = {3, 0}, p3 = {4, 0};
  Stack s;
  s.at = p0;
  s.follow(p1);
  s.follow(p2);
  s.follow(p3);  // [p0, p1, p2]
  s.follow(p0);  // push p3 drops p0 -> [p1, p2, p3]; p0 is gone, matches neither p2 nor p1
  EXPECT_EQ(s.depth, kCapacity);
  EXPECT_TRUE(eq(s.entries[2], p3));
}

TEST(LinkBackStack, ExplicitJumpClearsAndLaterLinksStartFresh) {
  Stack s;
  s.at = kText;
  s.follow(kNote);
  s.follow({9, 7});
  EXPECT_EQ(s.depth, 2);
  s.explicitJump({5, 0});  // TOC / percent / bookmark
  EXPECT_EQ(s.depth, 0);
  // A link whose landing matches a cleared entry is an ordinary jump.
  s.follow(kText);
  EXPECT_EQ(s.depth, 1);
  EXPECT_TRUE(eq(s.entries[0], {5, 0}));
}

}  // namespace
