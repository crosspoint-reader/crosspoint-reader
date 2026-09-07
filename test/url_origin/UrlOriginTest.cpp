#include <gtest/gtest.h>

#include "UrlOrigin.h"

// Regression coverage for the origin-comparison logic a redirect loop can use
// to decide whether to keep forwarding sensitive headers (e.g. Authorization)
// to a redirect target -- see HttpDownloader.cpp's runGet()/runGetWolf().
// urlOrigin() lives in its own header, independent of ESP-IDF/Arduino, with a
// std::string-based reference implementation kept here to guarantee
// refactor-safety of the heap-free std::string_view rewrite.

namespace {
// The behavior urlOrigin() is meant to match: a std::string-allocating
// version. Kept here, not in production code, purely so every case below can
// assert both implementations agree -- if this ever drifts from urlOrigin(),
// the heap-free rewrite silently changed origin-comparison semantics.
std::string referenceUrlOrigin(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return url;
  const size_t pathStart = url.find_first_of("/?#", schemeEnd + 3);
  return pathStart == std::string::npos ? url : url.substr(0, pathStart);
}
}  // namespace

TEST(UrlOrigin, ExtractsSchemeHostFromPathUrl) {
  EXPECT_EQ(urlOrigin("https://github.com/owner/repo/releases/download/v1/firmware.bin"), "https://github.com");
}

TEST(UrlOrigin, ExtractsSchemeHostFromCdnRedirectTarget) {
  // A common real-world redirect target class: a release-asset CDN on a
  // different host than the origin the download started from.
  EXPECT_EQ(urlOrigin("https://objects.githubusercontent.com/github-production-release-asset/1/firmware.bin?X-Amz="
                      "abc"),
            "https://objects.githubusercontent.com");
}

TEST(UrlOrigin, KeepsPortInOrigin) {
  EXPECT_EQ(urlOrigin("http://192.168.1.50:8080/opds/root.xml"), "http://192.168.1.50:8080");
}

TEST(UrlOrigin, NoPathStopsAtQueryString) {
  // A URL with no '/' at all after the scheme -- stopping at '/' alone would
  // fold the query string into the "origin" and misclassify a same-origin
  // redirect as cross-origin. This is the exact edge case the function's own
  // header comment calls out.
  EXPECT_EQ(urlOrigin("https://host?key=value"), "https://host");
}

TEST(UrlOrigin, NoPathStopsAtFragment) { EXPECT_EQ(urlOrigin("https://host#section"), "https://host"); }

TEST(UrlOrigin, BareOriginUnchanged) { EXPECT_EQ(urlOrigin("https://example.com"), "https://example.com"); }

TEST(UrlOrigin, MissingSchemeReturnsWholeInput) { EXPECT_EQ(urlOrigin("not-a-url"), "not-a-url"); }

TEST(UrlOrigin, SameOriginDifferentPathsCompareEqual) {
  EXPECT_EQ(urlOrigin("https://example.com/opds/a.xml"), urlOrigin("https://example.com/opds/b.xml"));
}

TEST(UrlOrigin, CrossOriginComparesUnequal) {
  EXPECT_NE(urlOrigin("https://github.com/owner/repo/releases/download/v1/firmware.bin"),
            urlOrigin("https://objects.githubusercontent.com/github-production-release-asset/1/firmware.bin"));
}

// Refactor-safety: the heap-free std::string_view implementation must never
// disagree with the std::string-returning implementation it replaced, across
// every case above plus a few more edge shapes.
TEST(UrlOrigin, MatchesReferenceImplementationAcrossShapes) {
  constexpr const char* const cases[] = {
      "https://github.com/owner/repo/releases/download/v1/firmware.bin",
      "https://objects.githubusercontent.com/github-production-release-asset/1/firmware.bin?X-Amz=abc",
      "http://192.168.1.50:8080/opds/root.xml",
      "https://host?key=value",
      "https://host#section",
      "https://example.com",
      "not-a-url",
      "",
      "https://",
  };
  for (const char* const c : cases) {
    EXPECT_EQ(std::string(urlOrigin(c)), referenceUrlOrigin(c)) << "mismatch for input: " << c;
  }
}
