#include <unity.h>

void setUp() {}
void tearDown() {}

void test_sanity() { TEST_ASSERT_EQUAL(2, 1 + 1); }

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_sanity);
  return UNITY_END();
}
