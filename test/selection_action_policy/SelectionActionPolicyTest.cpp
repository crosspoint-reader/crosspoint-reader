#include <gtest/gtest.h>

#include "clippings/SelectionActionPolicy.h"

using namespace selectionAction;

TEST(SelectionActionPolicy, SingleWordSkipsMenuAndLooksUp) {
  EXPECT_TRUE(runsWithoutMenu(1, true));
  EXPECT_EQ(defaultAction(1, true), Kind::Lookup);
}

TEST(SelectionActionPolicy, ShortPhrasesDefaultToLookupInMenu) {
  EXPECT_FALSE(runsWithoutMenu(2, true));
  EXPECT_EQ(defaultAction(2, true), Kind::Lookup);
}

TEST(SelectionActionPolicy, LongerSelectionsDefaultToClip) {
  EXPECT_FALSE(runsWithoutMenu(3, true));
  EXPECT_EQ(defaultAction(3, true), Kind::Clip);
  EXPECT_EQ(defaultAction(200, true), Kind::Clip);
}

TEST(SelectionActionPolicy, WithoutDictionaryAlwaysClipsDirectly) {
  EXPECT_TRUE(runsWithoutMenu(1, false));
  EXPECT_TRUE(runsWithoutMenu(5, false));
  EXPECT_EQ(defaultAction(1, false), Kind::Clip);
}
