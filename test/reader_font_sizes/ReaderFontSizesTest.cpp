// The size list behind every reader font-size control. The settings rows, the reader menu, the
// size buttons and the font a page is drawn with all go through it, so a stored size is shown,
// stepped from and rendered as the same size.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ReaderFontSizes.h"
#include "fontIds.h"

namespace {

using S = CrossPointSettings;

std::vector<int> asVector(const ReaderSizeList& list) { return {list.points, list.points + list.count}; }

std::vector<int> ladderPoints() {
  std::vector<int> pts;
  for (const auto& rung : S::FONT_SIZE_RUNGS) pts.push_back(rung.points);
  return pts;
}

TEST(ReaderSizeList, BuiltinIsTheLadderInAscendingOrder) {
  EXPECT_EQ(ladderPoints(), asVector(ReaderSizeList::builtin()));
}

TEST(ReaderSizeList, AnOfferedSizeSnapsToItself) {
  const auto list = ReaderSizeList::builtin();
  for (const auto& rung : S::FONT_SIZE_RUNGS) EXPECT_EQ(int{rung.points}, int{list.snap(rung.points)});
}

// A size chosen under an SD family (8 pt, say) read while Bookerly is selected.
TEST(ReaderSizeList, AnUnofferedSizeSnapsToTheNearest) {
  const auto list = ReaderSizeList::builtin();
  const int smallest = list.points[0];
  const int largest = list.points[list.count - 1];
  EXPECT_EQ(smallest, list.snap(static_cast<uint8_t>(smallest - 2)));
  EXPECT_EQ(smallest, list.snap(1));
  EXPECT_EQ(largest, list.snap(static_cast<uint8_t>(largest + 9)));
  EXPECT_EQ(largest, list.snap(255));
}

// Halfway between two sizes goes to the smaller. Built from the table: where a gap is even,
// (a + b) / 2 is a true tie.
TEST(ReaderSizeList, ATieGoesToTheSmallerSize) {
  const auto list = ReaderSizeList::builtin();
  for (uint8_t i = 1; i < list.count; ++i) {
    const int a = list.points[i - 1];
    const int b = list.points[i];
    if ((a + b) % 2 != 0) continue;
    EXPECT_EQ(a, list.snap(static_cast<uint8_t>((a + b) / 2))) << "between " << a << " and " << b;
  }
}

// Built-in faces are chosen by builtinRungForPoints(), the settings row by snap(). If the two
// ever disagreed, the row would read one size while the page was drawn in another.
TEST(ReaderSizeList, BuiltinSnapAgreesWithTheBuiltinFaceChosen) {
  const auto list = ReaderSizeList::builtin();
  for (int pt = 1; pt <= 255; ++pt) {
    const auto p = static_cast<uint8_t>(pt);
    EXPECT_EQ(int{list.snap(p)}, int{S::fontSizePoints(S::builtinRungForPoints(p))}) << pt << "pt";
  }
}

TEST(ReaderSizeList, IndexOfAnOfferedSizeIsItsPosition) {
  const auto list = ReaderSizeList::builtin();
  for (uint8_t i = 0; i < list.count; ++i) EXPECT_EQ(int{i}, int{list.indexOf(list.points[i])});
}

TEST(ReaderSizeList, StepsUpAndDownThroughEverySize) {
  const auto list = ReaderSizeList::builtin();
  for (uint8_t i = 1; i < list.count; ++i) {
    EXPECT_EQ(int{list.points[i]}, int{list.step(list.points[i - 1], 1)});
    EXPECT_EQ(int{list.points[i - 1]}, int{list.step(list.points[i], -1)});
  }
}

// A pinch that has reached the end should stay there. Wrapping would turn a continued pinch-out
// into the smallest text on screen.
TEST(ReaderSizeList, StepClampsRatherThanWrapping) {
  const auto list = ReaderSizeList::builtin();
  const uint8_t smallest = list.points[0];
  const uint8_t largest = list.points[list.count - 1];
  EXPECT_EQ(int{largest}, int{list.step(largest, 1)});
  EXPECT_EQ(int{largest}, int{list.step(largest, 99)});
  EXPECT_EQ(int{smallest}, int{list.step(smallest, -1)});
  EXPECT_EQ(int{smallest}, int{list.step(smallest, -99)});
  for (uint8_t i = 0; i < list.count; ++i) EXPECT_EQ(int{list.points[i]}, int{list.step(list.points[i], 0)});
}

// From a stored size the list does not offer, steps count from the size SHOWN for it.
TEST(ReaderSizeList, StepsFromAnUnofferedSizeStartAtItsNearest) {
  const auto list = ReaderSizeList::builtin();
  const auto below = static_cast<uint8_t>(list.points[0] - 2);
  EXPECT_EQ(int{list.points[1]}, int{list.step(below, 1)});
  EXPECT_EQ(int{list.points[0]}, int{list.step(below, -1)}) << "the smallest is already what is shown";
}

TEST(ReaderSizeList, NextWrapsFromTheLargestToTheSmallest) {
  const auto list = ReaderSizeList::builtin();
  for (uint8_t i = 1; i < list.count; ++i) EXPECT_EQ(int{list.points[i]}, int{list.next(list.points[i - 1])});
  EXPECT_EQ(int{list.points[0]}, int{list.next(list.points[list.count - 1])});
}

// --- an SD family's list -------------------------------------------------------------------------

std::vector<int> ladderAbove(const int pt) {
  std::vector<int> pts;
  for (const auto& rung : S::FONT_SIZE_RUNGS) {
    if (rung.points > pt) pts.push_back(rung.points);
  }
  return pts;
}

std::vector<int> concat(std::vector<int> a, const std::vector<int>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

// The request in #395: a family built below the ladder offers its own sizes, and the large-print
// sizes above it stay available, drawn from its largest file.
TEST(ReaderSizeListForFamily, OffersItsFilesThenTheLadderAboveTheLargest) {
  const uint8_t files[] = {9, 6, 8, 7};  // directory order, not sorted
  EXPECT_EQ(concat({6, 7, 8, 9}, ladderAbove(9)), asVector(ReaderSizeList::forFamily(files, 4)));
}

// A ladder size below the largest file would be drawn by shrinking a bigger face; the family's own
// smaller files are the better answer, so none is offered.
TEST(ReaderSizeListForFamily, NeverOffersALadderSizeBelowItsLargestFile) {
  const uint8_t files[] = {12, 13, 14, 16, 18};
  EXPECT_EQ(concat({12, 13, 14, 16, 18}, ladderAbove(18)), asVector(ReaderSizeList::forFamily(files, 5)));
}

// The upgrade case: a reader at the old smallest size (10 pt) picks a family whose smallest file is
// 12 pt. They see and get 12 pt -- not 10 pt shrunk from it.
TEST(ReaderSizeListForFamily, ASizeBelowTheSmallestFileSnapsUpToIt) {
  const uint8_t files[] = {12, 13, 14, 16, 18};
  EXPECT_EQ(12, int{ReaderSizeList::forFamily(files, 5).snap(10)});
}

TEST(ReaderSizeListForFamily, RepeatsAndOutOfRangeSizesAreSkipped) {
  const uint8_t files[] = {14, 14, 0, 200, 12, S::MAX_FONT_POINT_SIZE + 1};
  EXPECT_EQ(concat({12, 14}, ladderAbove(14)), asVector(ReaderSizeList::forFamily(files, 6)));
}

TEST(ReaderSizeListForFamily, AOneFileFamilyStillOffersLargePrint) {
  const uint8_t files[] = {11};
  EXPECT_EQ(concat({11}, ladderAbove(11)), asVector(ReaderSizeList::forFamily(files, 1)));
}

TEST(ReaderSizeListForFamily, AFamilyLargerThanTheLadderAddsNoLadderSizes) {
  const uint8_t files[] = {36, 20, 30};
  EXPECT_EQ((std::vector<int>{20, 30, 36}), asVector(ReaderSizeList::forFamily(files, 3)));
}

// An empty directory listing, or one with nothing usable, falls back to what a built-in family
// offers -- which is also what the reader draws with when the family cannot be used.
TEST(ReaderSizeListForFamily, NoUsableFilesMeansTheLadder) {
  EXPECT_EQ(asVector(ReaderSizeList::builtin()), asVector(ReaderSizeList::forFamily(nullptr, 0)));
  const uint8_t unusable[] = {0, 200};
  EXPECT_EQ(asVector(ReaderSizeList::builtin()), asVector(ReaderSizeList::forFamily(unusable, 2)));
}

// More sizes than the list holds: the smallest are kept, whatever order they arrive in.
TEST(ReaderSizeListForFamily, KeepsTheSmallestSizesWhenThereAreMoreThanFit) {
  constexpr int kExtra = 10;
  std::vector<uint8_t> files;
  for (int pt = ReaderSizeList::kCapacity + kExtra; pt >= 1; --pt) files.push_back(static_cast<uint8_t>(pt));
  const auto list = ReaderSizeList::forFamily(files.data(), files.size());
  ASSERT_EQ(int{ReaderSizeList::kCapacity}, int{list.count});
  EXPECT_EQ(1, int{list.points[0]});
  EXPECT_EQ(int{ReaderSizeList::kCapacity}, int{list.points[list.count - 1]});
}

TEST(FontPointSizeLabel, ReadsAsThePointSize) {
  EXPECT_EQ("7pt", fontPointSizeLabel(7));
  EXPECT_EQ("14pt", fontPointSizeLabel(14));
  EXPECT_EQ("127pt", fontPointSizeLabel(S::MAX_FONT_POINT_SIZE));
}

}  // namespace

TEST(ReaderSizeListNative, OffersEightPointBelowTheLadder) {
  const ReaderSizeList sizes = ReaderSizeList::native();
  ASSERT_EQ(CrossPointSettings::FONT_SIZE_RUNG_COUNT + 1, sizes.count);
  EXPECT_EQ(8, sizes.points[0]);
  for (int i = 0; i < CrossPointSettings::FONT_SIZE_RUNG_COUNT; ++i) {
    EXPECT_EQ(CrossPointSettings::FONT_SIZE_RUNGS[i].points, sizes.points[i + 1]);
  }
}

TEST(ReaderSizeListNative, TheSmallSizesSnapAndStepToEight) {
  const ReaderSizeList sizes = ReaderSizeList::native();
  EXPECT_EQ(8, sizes.snap(9));  // a tie goes to the smaller size
  EXPECT_EQ(8, sizes.snap(1));
  EXPECT_EQ(8, sizes.step(10, -1));
  EXPECT_EQ(26, sizes.next(24));
  EXPECT_EQ(8, sizes.next(26));  // the size button wraps to the new smallest
}

TEST(ReaderSizeListNative, BuiltInFamiliesStillStartAtTheLadder) { EXPECT_EQ(10, ReaderSizeList::builtin().snap(8)); }

// 20 pt has no faces of its own: it is the 18 pt faces drawn at x1.11, the way 22-26 pt are the same
// faces drawn larger (main.cpp registers every rung above the master like that). A stored 20 pt must
// still name a rung and an ID of its own, so it keeps reading at 20 pt and caches apart from 18 pt.
TEST(BuiltinReaderFaces, TwentyPointIsTheEighteenPointFacesScaled) {
  EXPECT_EQ(S::PT_20, S::builtinRungForPoints(20));
  EXPECT_EQ(20, int{S::fontSizePoints(S::PT_20)});
  ASSERT_EQ(18, int{S::BUILTIN_FACE_MAX_POINTS});
  EXPECT_FLOAT_EQ(20.0f / 18.0f, S::builtinFaceScale(20));
  EXPECT_NE(BOOKERLY_20_FONT_ID, BOOKERLY_18_FONT_ID);
  EXPECT_NE(NOTOSANS_20_FONT_ID, NOTOSANS_18_FONT_ID);
}

// The master and every rung below it draw their own faces at their own size; every rung above it is
// the master enlarged, never reduced and never drawn at x1.
TEST(BuiltinReaderFaces, OnlyTheRungsAboveTheMasterAreScaled) {
  for (const auto& rung : S::FONT_SIZE_RUNGS) {
    const float scale = S::builtinFaceScale(rung.points);
    if (rung.points <= S::BUILTIN_FACE_MAX_POINTS) {
      EXPECT_EQ(1.0f, scale) << int{rung.points} << "pt";
    } else {
      EXPECT_GT(scale, 1.0f) << int{rung.points} << "pt";
      EXPECT_FLOAT_EQ(static_cast<float>(rung.points) / S::BUILTIN_FACE_MAX_POINTS, scale) << int{rung.points} << "pt";
    }
  }
}
