// The S3 profile of radioVerdict: the numbers measured on an X4 Pro on 13/10/2026 with NimBLE's host
// allocated from PSRAM, and the start at Home. The default profile (X3) keeps its numbers.

#include <gtest/gtest.h>

#include "RadioPolicy.h"

namespace bleturner {
namespace {

// Measured on an X4 Pro (13/10/2026): Home before and after the stack came up.
constexpr Heap kHomeBefore{124752, 81908};
constexpr Heap kHomeAfter{78244, 36852};
// The same stack with its host in internal RAM left 31,732 largest: rolled back.
constexpr Heap kInternalAfter{56604, 31732};
// 13/10/2026 on an X4 Pro: a release build ("beta") and a probe build with small log buffers ("lite probe").
constexpr Heap kBetaHome{135112, 90100};
constexpr Heap kLiteHomeBefore{121216, 77812};
constexpr Heap kLiteHomeAfter{74712, 32756};  // 12 B short of the old 32,768: rolled back
constexpr Heap kLiteBookBefore{112588, 57332};
constexpr Heap kLiteBookAfter{66288, 31732};
constexpr Heap kPairingBefore{117360, 73716};  // Settings and the Bluetooth screen hold about 18 KB
constexpr Heap kPairingAfter{70840, 31732};    // the beta rolled back here: "Could not start Bluetooth"

RadioInputs home(const Phase phase) {
  RadioInputs in{};
  in.phase = phase;
  in.enabled = true;
  in.where = Where::Home;
  in.pageShown = true;
  in.hasBond = true;
  in.heap = kHomeBefore;
  return in;
}

TEST(RadioVerdictS3Test, TheNumbersAreTheMeasuredOnesPlusMargin) {
  EXPECT_EQ(kS3StackBytes, 46508u + 4096u);
  EXPECT_EQ(kS3Profile.stackBytes, kS3StackBytes);
  // On the S3 malloc and new above 4,096 B go to PSRAM (the inflate ring, the framebuffer-sized buffers), so
  // what the reader still wants from internal RAM is a few requests of at most 4,096 B: six of them, not the
  // 32 KiB block an X3 reader needs. The heaviest build on the host (5,000 anchors) asks 16,560 B live in
  // requests of that size, largest 4,096 B (HugeBookSection.S3InternalCensusOfTheHeaviestBuild).
  EXPECT_EQ(kS3InternalAllocMax, 4096u);
  // Six requests: the census counts neither malloc nor task stacks, so the room is wider than its figure.
  EXPECT_EQ(kS3Profile.minimumLargest, 6 * kS3InternalAllocMax);
  EXPECT_EQ(kS3Profile.minimumLargest, 24576u);
  EXPECT_EQ(kS3Profile.startFreeBytes, 75180u);
  EXPECT_EQ(kS3Profile.homeStartLargest, 71680u);
  EXPECT_EQ(kS3Profile.startFreeBytes, kS3StackBytes + kS3Profile.minimumLargest);
  // The stack takes 45,056 B from the largest block; a start at Home must leave the reader its room.
  EXPECT_EQ(kS3Profile.homeStartLargest, kS3Profile.minimumLargest + 45056u + 2048u);
  EXPECT_LE(kS3Profile.homeStartLargest, kHomeBefore.largestBlock) << "the unit's Home passes";
  EXPECT_LE(kS3Profile.homeStartLargest, kLiteHomeBefore.largestBlock) << "so does the lite probe's";
  EXPECT_LE(kS3Profile.homeStartLargest, kBetaHome.largestBlock) << "and the beta's";
  EXPECT_LE(kS3Profile.startFreeBytes, kHomeBefore.freeBytes);
}

TEST(RadioVerdictS3Test, TheDefaultProfileIsStillTheX3One) {
  EXPECT_EQ(kStackBytes, 49152u);
  EXPECT_EQ(kStartFreeBytes, 81920u);
  EXPECT_EQ(kX3Profile.stackBytes, kStackBytes);
  EXPECT_EQ(kX3Profile.startFreeBytes, kStartFreeBytes);
  EXPECT_EQ(kX3Profile.homeStartLargest, 0u);
  EXPECT_EQ(&activeProfile(), &kX3Profile) << "this test binary is built without FREEINK_BLE_STACK_IN_PSRAM";
}

TEST(RadioVerdictS3Test, HomeMayAskOnlyOnS3WithABondOnAShownPage) {
  EXPECT_EQ(radioVerdict(home(Phase::Idle), kS3Profile), Why::Ok);
  EXPECT_EQ(radioVerdict(home(Phase::Idle), kX3Profile), Why::NotReading);
  EXPECT_EQ(radioVerdict(home(Phase::Idle)), Why::NotReading);
  RadioInputs in = home(Phase::Idle);
  in.hasBond = false;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::NotReading);
  in = home(Phase::Idle);
  in.pageShown = false;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::PageNotShown);
  in = home(Phase::Idle);
  in.enabled = false;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::Off);
  in = home(Phase::Idle);
  in.storageBusy = true;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::StorageBusy);
  in = home(Phase::Idle);
  in.idleStopped = true;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::IdleNoLink);
}

TEST(RadioVerdictS3Test, StartingAtHomeNeedsTheBlockTheReaderWillStillWant) {
  EXPECT_EQ(radioVerdict(home(Phase::BeforeStart), kS3Profile), Why::Ok);
  RadioInputs in = home(Phase::BeforeStart);
  in.heap.largestBlock = kS3Profile.homeStartLargest - 1;
  // Not HeapInPieces: that verdict counts toward a restart into the book, which makes no sense at Home.
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::HeapLow);
  in = home(Phase::BeforeStart);
  in.heap.freeBytes = kS3Profile.startFreeBytes - 1;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::HeapLow);
  in = home(Phase::BeforeStart);
  in.heap = kBetaHome;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::Ok);
  in.heap = kLiteHomeBefore;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::Ok);
}

TEST(RadioVerdictS3Test, InTheBookTheSamePieceRulesApplyWithTheS3Numbers) {
  RadioInputs in = home(Phase::BeforeStart);
  in.where = Where::Reader;
  in.heap = {kS3Profile.startFreeBytes, kS3Profile.minimumLargest};
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::Ok);
  in.heap.largestBlock = kS3Profile.minimumLargest - 1;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::HeapInPieces) << "the silent restart into the book still applies";
  in.heap = {kS3Profile.startFreeBytes - 1, kS3Profile.minimumLargest};
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::HeapInPieces);
  in.heap = {kMinimumFreeBytes - 1, kS3Profile.minimumLargest};
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::HeapLow);
  // Measured in the book on the lite probe: it came up with 31,732 left and was rolled back for 1,036 B.
  in.phase = Phase::BeforeStart;
  in.heap = kLiteBookBefore;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::Ok);
  in.phase = Phase::JustStarted;
  in.heap = kLiteBookAfter;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::Ok);
  EXPECT_EQ(radioVerdict(in, kX3Profile), Why::HeapInPieces) << "an X3 reader would not have kept it";
}

TEST(RadioVerdictS3Test, TheStackIsKeptOrRolledBackOnTheLargestBlockAfterIt) {
  RadioInputs in = home(Phase::JustStarted);
  in.heap = kHomeAfter;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::Ok) << "PSRAM host: 36,852 left";
  in.heap = kLiteHomeAfter;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::Ok) << "the lite probe's Home, 32,756 left, was 12 B short";
  EXPECT_EQ(radioVerdict(in, kX3Profile), Why::HeapInPieces);
  in.heap = {kS3Profile.startFreeBytes, kS3Profile.minimumLargest - 1};
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::HeapInPieces) << "below what six internal requests need";
  in.heap = kInternalAfter;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::Ok) << "31,732 left is plenty for the S3 reader";
}

// The four situations measured on 13/10/2026 against the final numbers, and the pairing screen's
// start too: Idle/BeforeStart/JustStarted verdicts, nothing rolled back.
TEST(RadioVerdictS3Test, TheFourMeasuredSituationsAndThePairingScreenAllPass) {
  struct Row {
    const char* name;
    Where where;
    Heap before, after;
  };
  const Row rows[] = {
      {"Home, beta", Where::Home, kBetaHome, {kBetaHome.freeBytes - 46508, kBetaHome.largestBlock - 45056}},
      {"Home, lite", Where::Home, kLiteHomeBefore, kLiteHomeAfter},
      {"in the book, lite", Where::Reader, kLiteBookBefore, kLiteBookAfter},
      {"pairing screen", Where::PairingScreen, kPairingBefore, kPairingAfter},
  };
  for (const Row& row : rows) {
    RadioInputs in{};
    in.enabled = true;
    in.where = row.where;
    in.pageShown = true;
    in.hasBond = true;
    in.phase = Phase::BeforeStart;
    in.heap = row.before;
    EXPECT_EQ(radioVerdict(in, kS3Profile), Why::Ok) << row.name << " before";
    in.phase = Phase::JustStarted;
    in.heap = row.after;
    EXPECT_EQ(radioVerdict(in, kS3Profile), Why::Ok) << row.name << " after";
  }
  // The pairing screen starts by the same rule as the book (free and largest for the stack), not by the Home
  // rule; its 73,716 would pass the Home largest (71,680) too.
  EXPECT_GE(kPairingBefore.largestBlock, kS3Profile.homeStartLargest);
  EXPECT_GE(kPairingBefore.largestBlock, kS3Profile.minimumLargest + 45056u)
      << "the stack's cut still leaves the floor";
}

// The first pairing: the radio has to come up from the Bluetooth screen, which sits on Settings.
TEST(RadioVerdictS3Test, TheFirstPairingStartsFromTheSettingsScreen) {
  RadioInputs in{};
  in.phase = Phase::Idle;
  in.enabled = true;
  in.where = Where::PairingScreen;
  in.heap = kPairingBefore;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::Ok);
  in.phase = Phase::BeforeStart;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::Ok) << "117,360 free and 73,716 largest";
  in.phase = Phase::JustStarted;
  in.heap = kPairingAfter;
  EXPECT_EQ(radioVerdict(in, kS3Profile), Why::Ok) << "the beta was rolled back here by the X3 number";
  EXPECT_EQ(radioVerdict(in, kX3Profile), Why::HeapInPieces) << "(the old behavior, kept for the X3)";
}

}  // namespace
}  // namespace bleturner
