#pragma once
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_FAIL(msg)                                                              \
  do {                                                                              \
    std::cerr << "  FAIL: " << __FILE__ << ":" << __LINE__ << " — " << msg << "\n"; \
    ++g_testsFailed;                                                                \
  } while (0)

#define TEST_PASS()  \
  do {               \
    ++g_testsPassed; \
  } while (0)

#define ASSERT_TRUE(expr)                           \
  do {                                              \
    if (!(expr)) {                                  \
      TEST_FAIL(#expr " expected true, got false"); \
    } else {                                        \
      TEST_PASS();                                  \
    }                                               \
  } while (0)

#define ASSERT_FALSE(expr)                          \
  do {                                              \
    if ((expr)) {                                   \
      TEST_FAIL(#expr " expected false, got true"); \
    } else {                                        \
      TEST_PASS();                                  \
    }                                               \
  } while (0)

#define ASSERT_EQ(a, b)                                                                                              \
  do {                                                                                                               \
    if ((a) != (b)) {                                                                                                \
      std::cerr << "  FAIL: " << __FILE__ << ":" << __LINE__ << " — " << #a << " != " << #b << " (" << (a) << " vs " \
                << (b) << ")\n";                                                                                     \
      ++g_testsFailed;                                                                                               \
    } else {                                                                                                         \
      TEST_PASS();                                                                                                   \
    }                                                                                                                \
  } while (0)

#define ASSERT_NE(a, b)                                                                              \
  do {                                                                                               \
    if ((a) == (b)) {                                                                                \
      std::cerr << "  FAIL: " << __FILE__ << ":" << __LINE__ << " — " << #a << " == " << #b << "\n"; \
      ++g_testsFailed;                                                                               \
    } else {                                                                                         \
      TEST_PASS();                                                                                   \
    }                                                                                                \
  } while (0)

#define ASSERT_STREQ(a, b)                                            \
  do {                                                                \
    const std::string _a(a);                                          \
    const std::string _b(b);                                          \
    if (_a != _b) {                                                   \
      std::cerr << "  FAIL: " << __FILE__ << ":" << __LINE__ << " — " \
                << "\"" << _a << "\" != \"" << _b << "\"\n";          \
      ++g_testsFailed;                                                \
    } else {                                                          \
      TEST_PASS();                                                    \
    }                                                                 \
  } while (0)

#define ASSERT_NEAR(a, b, eps)                                                                          \
  do {                                                                                                  \
    if (std::fabs((a) - (b)) > (eps)) {                                                                 \
      std::cerr << "  FAIL: " << __FILE__ << ":" << __LINE__ << " — " << #a << " ≈ " << #b << " (diff " \
                << std::fabs((a) - (b)) << " > " << (eps) << ")\n";                                     \
      ++g_testsFailed;                                                                                  \
    } else {                                                                                            \
      TEST_PASS();                                                                                      \
    }                                                                                                   \
  } while (0)

#define TEST_SUMMARY()                                                                                            \
  do {                                                                                                            \
    std::cout << (g_testsFailed == 0 ? "OK" : "FAILED") << " — " << g_testsPassed << " passed, " << g_testsFailed \
              << " failed\n";                                                                                     \
    return g_testsFailed == 0 ? 0 : 1;                                                                            \
  } while (0)

#define RUN_TEST(fn)                                                   \
  do {                                                                 \
    std::cout << "  " << #fn << "... ";                                \
    const int _before = g_testsFailed;                                 \
    fn();                                                              \
    std::cout << (g_testsFailed == _before ? "ok" : "FAILED") << "\n"; \
  } while (0)
