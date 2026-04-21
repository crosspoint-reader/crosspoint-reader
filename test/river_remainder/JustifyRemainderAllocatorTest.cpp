#include <cstdint>
#include <cstdio>
#include <vector>

#include "lib/Epub/Epub/JustifyRemainderAllocator.h"

static int testsPassed = 0;
static int testsFailed = 0;

#define ASSERT_EQ(a, b)                                                                 \
  do {                                                                                  \
    const auto _assertEqLhs = (a);                                                      \
    const auto _assertEqRhs = (b);                                                      \
    if (_assertEqLhs != _assertEqRhs) {                                                 \
      fprintf(stderr, "  FAIL: %s:%d: %s == %d, expected %d\n", __FILE__, __LINE__, #a, \
              static_cast<int>(_assertEqLhs), static_cast<int>(_assertEqRhs));          \
      testsFailed++;                                                                    \
      return;                                                                           \
    }                                                                                   \
  } while (0)

#define ASSERT_TRUE(cond)                                                \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      testsFailed++;                                                     \
      return;                                                            \
    }                                                                    \
  } while (0)

#define PASS() testsPassed++

bool assertVectorEq(const std::vector<int>& actual, const std::vector<int>& expected) {
  if (static_cast<int>(actual.size()) != static_cast<int>(expected.size())) {
    fprintf(stderr, "  FAIL: %s:%d: size mismatch %zu != %zu\n", __FILE__, __LINE__, actual.size(), expected.size());
    testsFailed++;
    return false;
  }
  for (size_t i = 0; i < expected.size(); ++i) {
    if (actual[i] != expected[i]) {
      fprintf(stderr, "  FAIL: %s:%d: actual[%zu] == %d, expected %d\n", __FILE__, __LINE__, i, actual[i], expected[i]);
      testsFailed++;
      return false;
    }
  }
  return true;
}

void testZeroRemainder() {
  printf("testZeroRemainder...\n");
  const auto bonuses = allocateJustifyRemainderBonuses({10, 20}, {5, 15, 25}, 0);
  if (!assertVectorEq(bonuses, {0, 0, 0})) return;
  PASS();
}

void testNoPreviousLineGapsDistributesLeftToRight() {
  printf("testNoPreviousLineGapsDistributesLeftToRight...\n");
  const auto bonuses = allocateJustifyRemainderBonuses({}, {10, 20, 30}, 5);
  if (!assertVectorEq(bonuses, {2, 2, 1})) return;
  PASS();
}

void testOnePreviousGapFavorsFarthestCandidate() {
  printf("testOnePreviousGapFavorsFarthestCandidate...\n");
  const auto bonuses = allocateJustifyRemainderBonuses({10}, {8, 30, 50}, 1);
  if (!assertVectorEq(bonuses, {0, 0, 1})) return;
  PASS();
}

void testMultipleCandidatesWithMultipleRemainderPixels() {
  printf("testMultipleCandidatesWithMultipleRemainderPixels...\n");
  const auto bonuses = allocateJustifyRemainderBonuses({10, 50}, {5, 20, 35, 70}, 3);
  if (!assertVectorEq(bonuses, {0, 0, 0, 3})) return;
  PASS();
}

void testTieBreakPrefersLowerCurrentBonus() {
  printf("testTieBreakPrefersLowerCurrentBonus...\n");
  const auto bonuses = allocateJustifyRemainderBonuses({20}, {10, 30}, 3);
  if (!assertVectorEq(bonuses, {2, 1})) return;
  PASS();
}

void testTieBreakFallsBackToLeftToRight() {
  printf("testTieBreakFallsBackToLeftToRight...\n");
  const auto bonuses = allocateJustifyRemainderBonuses({20}, {10, 30}, 2);
  if (!assertVectorEq(bonuses, {1, 1})) return;
  PASS();
}

void testEmptyCandidates() {
  printf("testEmptyCandidates...\n");
  const auto bonuses = allocateJustifyRemainderBonuses({10, 20}, {}, 4);
  ASSERT_TRUE(bonuses.empty());
  PASS();
}

void testSingleCandidateGetsAllRemainder() {
  printf("testSingleCandidateGetsAllRemainder...\n");
  const auto bonuses = allocateJustifyRemainderBonuses({10, 30}, {25}, 4);
  ASSERT_EQ(bonuses.size(), static_cast<size_t>(1));
  if (!assertVectorEq(bonuses, {4})) return;
  PASS();
}

void testEquidistantCandidatesUseTieBreaker() {
  // prev={10,30}, cand={20,40}: both cand equidistant from nearest prev (distance 10 each).
  // Pixel 1: tie on distance, tie on bonus(0,0) -> left-to-right -> idx 0.
  // Pixel 2: tie on distance, bonuses are (1,0) -> lower-bonus rule -> idx 1.
  // Final bonuses: {1, 1}.
  printf("testEquidistantCandidatesUseTieBreaker...\n");
  const auto bonuses = allocateJustifyRemainderBonuses({10, 30}, {20, 40}, 2);
  if (!assertVectorEq(bonuses, {1, 1})) return;
  PASS();
}

int main() {
  testZeroRemainder();
  testNoPreviousLineGapsDistributesLeftToRight();
  testOnePreviousGapFavorsFarthestCandidate();
  testMultipleCandidatesWithMultipleRemainderPixels();
  testTieBreakPrefersLowerCurrentBonus();
  testTieBreakFallsBackToLeftToRight();
  testEmptyCandidates();
  testSingleCandidateGetsAllRemainder();
  testEquidistantCandidatesUseTieBreaker();

  printf("\nTests passed: %d\n", testsPassed);
  printf("Tests failed: %d\n", testsFailed);

  return testsFailed == 0 ? 0 : 1;
}
