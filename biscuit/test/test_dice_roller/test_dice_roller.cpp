// test/tests/test_dice_roller.cpp
// Tests pure logic of DiceRollerActivity: roll ranges, totals
// Run: pio test -e native -f test_dice_roller

#include <unity.h>
#include <cstdlib>
#include <cstdint>
#include <vector>

// ---- extracted logic (no hardware dependency) ----

static const int DIE_TYPES[] = {4, 6, 8, 10, 12, 20, 100};
static const int NUM_DIE_TYPES = 7;

uint32_t randomRange(uint32_t max) { return (rand() % max) + 1; }

struct RollResult {
  std::vector<int> dice;
  int total;
};

RollResult doRoll(int dieTypeIndex, int dieCount) {
  RollResult r;
  r.total = 0;
  int sides = DIE_TYPES[dieTypeIndex];
  for (int i = 0; i < dieCount; i++) {
    int val = (int)randomRange(sides);
    r.dice.push_back(val);
    r.total += val;
  }
  return r;
}

// ---- tests ----

void test_d6_single_roll_in_range() {
  for (int trial = 0; trial < 100; trial++) {
    auto r = doRoll(1, 1);  // 1d6
    TEST_ASSERT_EQUAL(1, (int)r.dice.size());
    TEST_ASSERT_GREATER_OR_EQUAL(1, r.dice[0]);
    TEST_ASSERT_LESS_OR_EQUAL(6, r.dice[0]);
    TEST_ASSERT_EQUAL(r.dice[0], r.total);
  }
}

void test_2d6_roll_range() {
  for (int trial = 0; trial < 100; trial++) {
    auto r = doRoll(1, 2);  // 2d6
    TEST_ASSERT_EQUAL(2, (int)r.dice.size());
    TEST_ASSERT_GREATER_OR_EQUAL(2, r.total);
    TEST_ASSERT_LESS_OR_EQUAL(12, r.total);
    TEST_ASSERT_EQUAL(r.dice[0] + r.dice[1], r.total);
  }
}

void test_d4_range() {
  for (int trial = 0; trial < 50; trial++) {
    auto r = doRoll(0, 1);  // 1d4
    TEST_ASSERT_GREATER_OR_EQUAL(1, r.dice[0]);
    TEST_ASSERT_LESS_OR_EQUAL(4, r.dice[0]);
  }
}

void test_d20_range() {
  for (int trial = 0; trial < 100; trial++) {
    auto r = doRoll(5, 1);  // 1d20
    TEST_ASSERT_GREATER_OR_EQUAL(1, r.dice[0]);
    TEST_ASSERT_LESS_OR_EQUAL(20, r.dice[0]);
  }
}

void test_d100_range() {
  for (int trial = 0; trial < 100; trial++) {
    auto r = doRoll(6, 1);  // 1d100
    TEST_ASSERT_GREATER_OR_EQUAL(1, r.dice[0]);
    TEST_ASSERT_LESS_OR_EQUAL(100, r.dice[0]);
  }
}

void test_6d6_total_range() {
  for (int trial = 0; trial < 50; trial++) {
    auto r = doRoll(1, 6);  // 6d6
    TEST_ASSERT_EQUAL(6, (int)r.dice.size());
    TEST_ASSERT_GREATER_OR_EQUAL(6, r.total);
    TEST_ASSERT_LESS_OR_EQUAL(36, r.total);
  }
}

void test_total_equals_sum() {
  auto r = doRoll(1, 4);  // 4d6
  int sum = 0;
  for (int v : r.dice) sum += v;
  TEST_ASSERT_EQUAL(sum, r.total);
}

void test_all_die_types_valid() {
  for (int t = 0; t < NUM_DIE_TYPES; t++) {
    for (int trial = 0; trial < 20; trial++) {
      auto r = doRoll(t, 1);
      TEST_ASSERT_GREATER_OR_EQUAL(1, r.dice[0]);
      TEST_ASSERT_LESS_OR_EQUAL(DIE_TYPES[t], r.dice[0]);
    }
  }
}

// ---- main ----
void setUp() { srand(42); }
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_d6_single_roll_in_range);
  RUN_TEST(test_2d6_roll_range);
  RUN_TEST(test_d4_range);
  RUN_TEST(test_d20_range);
  RUN_TEST(test_d100_range);
  RUN_TEST(test_6d6_total_range);
  RUN_TEST(test_total_equals_sum);
  RUN_TEST(test_all_die_types_valid);
  return UNITY_END();
}
