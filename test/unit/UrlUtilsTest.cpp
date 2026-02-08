#include "src/util/UrlUtils.h"
#include "test/test_harness.h"

// --- isHttpsUrl ---

void testIsHttps() {
  ASSERT_TRUE(UrlUtils::isHttpsUrl("https://example.com"));
  ASSERT_TRUE(UrlUtils::isHttpsUrl("https://example.com/path"));
  ASSERT_FALSE(UrlUtils::isHttpsUrl("http://example.com"));
  ASSERT_FALSE(UrlUtils::isHttpsUrl("ftp://example.com"));
  ASSERT_FALSE(UrlUtils::isHttpsUrl("example.com"));
  ASSERT_FALSE(UrlUtils::isHttpsUrl(""));
}

// --- ensureProtocol ---

void testEnsureProtocol() {
  ASSERT_STREQ(UrlUtils::ensureProtocol("example.com"), "http://example.com");
  ASSERT_STREQ(UrlUtils::ensureProtocol("http://example.com"), "http://example.com");
  ASSERT_STREQ(UrlUtils::ensureProtocol("https://example.com"), "https://example.com");
  ASSERT_STREQ(UrlUtils::ensureProtocol("ftp://files.com"), "ftp://files.com");
}

// --- extractHost ---

void testExtractHost() {
  ASSERT_STREQ(UrlUtils::extractHost("http://example.com/path/to/thing"), "http://example.com");
  ASSERT_STREQ(UrlUtils::extractHost("https://example.com"), "https://example.com");
  ASSERT_STREQ(UrlUtils::extractHost("https://example.com/"), "https://example.com");
  ASSERT_STREQ(UrlUtils::extractHost("example.com/path"), "example.com");
  ASSERT_STREQ(UrlUtils::extractHost("example.com"), "example.com");
}

// --- buildUrl ---

void testBuildUrlAbsolutePath() {
  ASSERT_STREQ(UrlUtils::buildUrl("http://example.com/catalog", "/new/path"), "http://example.com/new/path");
}

void testBuildUrlRelativePath() {
  ASSERT_STREQ(UrlUtils::buildUrl("http://example.com/catalog", "books"), "http://example.com/catalog/books");
  ASSERT_STREQ(UrlUtils::buildUrl("http://example.com/catalog/", "books"), "http://example.com/catalog/books");
}

void testBuildUrlEmptyPath() { ASSERT_STREQ(UrlUtils::buildUrl("example.com", ""), "http://example.com"); }

void testBuildUrlNoProtocol() { ASSERT_STREQ(UrlUtils::buildUrl("example.com", "/path"), "http://example.com/path"); }

int main() {
  std::cout << "UrlUtilsTest\n";
  RUN_TEST(testIsHttps);
  RUN_TEST(testEnsureProtocol);
  RUN_TEST(testExtractHost);
  RUN_TEST(testBuildUrlAbsolutePath);
  RUN_TEST(testBuildUrlRelativePath);
  RUN_TEST(testBuildUrlEmptyPath);
  RUN_TEST(testBuildUrlNoProtocol);
  TEST_SUMMARY();
}
