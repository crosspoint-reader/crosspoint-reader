#include <gtest/gtest.h>

#include "activities/reader/LinkNoteTitle.h"

using bleturner::LinkNote;

TEST(LinkNoteTitle, ShownOnlyWhereTheStatusBarDrawsATitle) {
  LinkNoteTitle t;
  EXPECT_EQ(t.draw(true, LinkNote::Connecting), LinkNote::Connecting);
  EXPECT_EQ(t.draw(true, LinkNote::Failed), LinkNote::Failed);
  EXPECT_EQ(t.draw(true, LinkNote::None), LinkNote::None);
  EXPECT_EQ(t.draw(false, LinkNote::Connecting), LinkNote::None);
  EXPECT_EQ(t.draw(false, LinkNote::Failed), LinkNote::None);
}

TEST(LinkNoteTitle, NothingDrawnYetNeverRepaints) {
  LinkNoteTitle t;
  EXPECT_FALSE(t.repaint(LinkNote::Connecting));
  EXPECT_FALSE(t.repaint(LinkNote::Failed));
}

TEST(LinkNoteTitle, RemoteLinkedGivesTheTitleBackOnce) {
  LinkNoteTitle t;
  t.draw(true, LinkNote::Connecting);
  EXPECT_FALSE(t.repaint(LinkNote::Connecting));
  EXPECT_TRUE(t.repaint(LinkNote::None));
  // Asked once: the loop keeps passing until the new paint draws the slot again.
  EXPECT_FALSE(t.repaint(LinkNote::None));
  t.draw(true, LinkNote::None);
  EXPECT_FALSE(t.repaint(LinkNote::None));
}

TEST(LinkNoteTitle, FailureReplacesTheConnectingNote) {
  LinkNoteTitle t;
  t.draw(true, LinkNote::Connecting);
  EXPECT_TRUE(t.repaint(LinkNote::Failed));
  t.draw(true, LinkNote::Failed);
  EXPECT_FALSE(t.repaint(LinkNote::Failed));
  // The next page turn reads the failure; its own paint shows the title.
  EXPECT_TRUE(t.repaint(LinkNote::None));
}

TEST(LinkNoteTitle, HiddenTitleNeverRepaints) {
  LinkNoteTitle t;
  t.draw(false, LinkNote::Connecting);
  EXPECT_FALSE(t.repaint(LinkNote::None));
  EXPECT_FALSE(t.repaint(LinkNote::Failed));
  t.draw(false, LinkNote::None);
  EXPECT_FALSE(t.repaint(LinkNote::Connecting));
}

TEST(LinkNoteTitle, PaintWithoutAStatusBarDoesNotLoop) {
  LinkNoteTitle t;
  t.draw(true, LinkNote::Connecting);
  EXPECT_TRUE(t.repaint(LinkNote::None));
  // That paint drew no status bar (a screen without one): no further repaint is asked.
  for (int i = 0; i < 3; ++i) EXPECT_FALSE(t.repaint(LinkNote::Failed));
}
