// The S31 profile of radioVerdict: the numbers measured on a Metalio Plus on 02/11/2026 with ESP-IDF's
// prebuilt host and the controller's PSRAM-safe blocks in PSRAM (src/platform/S31NimbleMemory.cpp).

#include <gtest/gtest.h>

#include "RadioPolicy.h"

namespace bleturner {
namespace {

// Measured on a Metalio Plus (02/11/2026): the Bluetooth screen and a book, before and after the stack.
constexpr Heap kPairingBefore{126904, 94196};
constexpr Heap kPairingAfter{91564, 61428};
constexpr Heap kBookBefore{125516, 55284};
constexpr Heap kBookAfter{90484, 55284};

RadioInputs at(const Where where, const Phase phase, const Heap heap) {
  RadioInputs in{};
  in.phase = phase;
  in.enabled = true;
  in.where = where;
  in.pageShown = true;
  in.hasBond = true;
  in.heap = heap;
  return in;
}

TEST(RadioVerdictS31Test, TheNumbersAreTheMeasuredOnesPlusMargin) {
  EXPECT_EQ(kS31StackBytes, 35340u + 4096u);
  EXPECT_EQ(kS31Profile.stackBytes, kS31StackBytes);
  EXPECT_EQ(kPairingBefore.freeBytes - kPairingAfter.freeBytes, 35340u);
  EXPECT_EQ(kPairingBefore.largestBlock - kPairingAfter.largestBlock, kS31StackLargestDrop);
  // The S31 sends malloc and new above 4,096 B to PSRAM as the S3 does, so the reader's room is the S3's.
  EXPECT_EQ(kS31Profile.minimumLargest, kS3MinimumLargest);
  EXPECT_EQ(kS31Profile.startFreeBytes, kS31StackBytes + kS3MinimumLargest);
  EXPECT_EQ(kS31Profile.homeStartLargest, kS3MinimumLargest + kS31StackLargestDrop + 2048u);
  EXPECT_EQ(&activeProfile(), &kX3Profile) << "this test binary is built without FREEINK_BLE_STACK_S31";
}

TEST(RadioVerdictS31Test, TheMeasuredStartsPass) {
  EXPECT_EQ(radioVerdict(at(Where::PairingScreen, Phase::BeforeStart, kPairingBefore), kS31Profile), Why::Ok);
  EXPECT_EQ(radioVerdict(at(Where::PairingScreen, Phase::JustStarted, kPairingAfter), kS31Profile), Why::Ok);
  EXPECT_EQ(radioVerdict(at(Where::Reader, Phase::BeforeStart, kBookBefore), kS31Profile), Why::Ok);
  EXPECT_EQ(radioVerdict(at(Where::Reader, Phase::JustStarted, kBookAfter), kS31Profile), Why::Ok);
  // The Bluetooth screen's heap is Home's or less: a start at Home fits.
  EXPECT_EQ(radioVerdict(at(Where::Home, Phase::BeforeStart, kPairingBefore), kS31Profile), Why::Ok);
}

TEST(RadioVerdictS31Test, TheS3NumbersWouldRefuseAnS31HomeThatFits) {
  // The S31 stack costs less than the S3's, so its gates are lower; with them an S31 Home with less
  // than the S3 asks for still starts the radio.
  const Heap home{kS31Profile.startFreeBytes, kS31Profile.homeStartLargest};
  EXPECT_EQ(radioVerdict(at(Where::Home, Phase::BeforeStart, home), kS31Profile), Why::Ok);
  EXPECT_EQ(radioVerdict(at(Where::Home, Phase::BeforeStart, home), kS3Profile), Why::HeapLow);
  RadioInputs in = at(Where::Home, Phase::BeforeStart, home);
  in.heap.largestBlock -= 1;
  EXPECT_EQ(radioVerdict(in, kS31Profile), Why::HeapLow);
}

// A chapter build runs beside the S31's stack: the remote stays linked through a chapter change.
TEST(RadioVerdictS31Test, AChapterBuildFitsBesideTheS31StackOnly) {
  EXPECT_TRUE(kS31Profile.buildsBesideStack);
  EXPECT_FALSE(kS3Profile.buildsBesideStack);
  EXPECT_FALSE(kX3Profile.buildsBesideStack);
  EXPECT_TRUE(buildFitsBesideRadio(kBookAfter, kS31Profile)) << "measured in a book with the radio up";
  EXPECT_TRUE(buildFitsBesideRadio(kPairingAfter, kS31Profile));
  EXPECT_FALSE(buildFitsBesideRadio(kBookAfter, kS3Profile));
  EXPECT_FALSE(buildFitsBesideRadio(kBookAfter, kX3Profile));
  // More than the 55 KB an X3 build ran out of memory in, and the reader's room in one block.
  EXPECT_GT(kBuildBesideRadioFreeBytes, 55u * 1024u);
  EXPECT_FALSE(buildFitsBesideRadio({kBuildBesideRadioFreeBytes - 1, kBookAfter.largestBlock}, kS31Profile));
  EXPECT_FALSE(buildFitsBesideRadio({kBookAfter.freeBytes, kS31Profile.minimumLargest - 1}, kS31Profile));
  EXPECT_TRUE(buildFitsBesideRadio({kBuildBesideRadioFreeBytes, kS31Profile.minimumLargest}, kS31Profile));
}

}  // namespace
}  // namespace bleturner
