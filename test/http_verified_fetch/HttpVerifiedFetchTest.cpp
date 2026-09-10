#include <gtest/gtest.h>

#include "HttpVerifiedFetch.h"

namespace {

// A stand-in for a real PEM bundle; only its emptiness matters here.
constexpr const char* kAnchors = "-----BEGIN CERTIFICATE-----\n";

TEST(UrlIsHttps, AcceptsLowercaseScheme) { EXPECT_TRUE(urlIsHttps("https://api.github.com/repos/x/releases/latest")); }

// RFC 3986 section 3.1: schemes are case-insensitive, and esp_http_client
// parses them that way. A byte-exact prefix test would refuse this URL while
// claiming to enforce transport security.
TEST(UrlIsHttps, AcceptsUppercaseAndMixedCaseScheme) {
  EXPECT_TRUE(urlIsHttps("HTTPS://api.github.com/"));
  EXPECT_TRUE(urlIsHttps("HtTpS://api.github.com/"));
}

TEST(UrlIsHttps, RejectsPlaintextAndOtherSchemes) {
  EXPECT_FALSE(urlIsHttps("http://api.github.com/"));
  EXPECT_FALSE(urlIsHttps("HTTP://api.github.com/"));
  EXPECT_FALSE(urlIsHttps("ftp://api.github.com/"));
  EXPECT_FALSE(urlIsHttps("//api.github.com/"));
  EXPECT_FALSE(urlIsHttps("api.github.com/"));
}

TEST(UrlIsHttps, RejectsTruncatedSchemeWithoutReadingPastTheEnd) {
  EXPECT_FALSE(urlIsHttps(""));
  EXPECT_FALSE(urlIsHttps("h"));
  EXPECT_FALSE(urlIsHttps("https:/"));
  EXPECT_TRUE(urlIsHttps("https://"));
}

// --- Precondition: builds with a default trust store (esp_crt_bundle) ---

TEST(VerifiedFetchRefusal, WithoutAnchorRequirementHttpsIsAllowed) {
  EXPECT_EQ(verifiedFetchRefusal("https://api.github.com/", kAnchors, false), nullptr);
  EXPECT_EQ(verifiedFetchRefusal("HTTPS://api.github.com/", kAnchors, false), nullptr);
  // No anchors needed when the platform verifies against its own bundle.
  EXPECT_EQ(verifiedFetchRefusal("https://api.github.com/", nullptr, false), nullptr);
}

// The https-only gate is not conditional on the TLS backend: it must hold on
// every build, or a "verified" fetch of an http:// URL goes out in clear.
TEST(VerifiedFetchRefusal, WithoutAnchorRequirementPlaintextIsStillRefused) {
  EXPECT_NE(verifiedFetchRefusal("http://api.github.com/", kAnchors, false), nullptr);
  EXPECT_NE(verifiedFetchRefusal("http://api.github.com/", nullptr, false), nullptr);
}

// --- Precondition: builds with no default trust store (wolfSSL) ---

TEST(VerifiedFetchRefusal, WithAnchorRequirementHttpsPlusAnchorsIsAllowed) {
  EXPECT_EQ(verifiedFetchRefusal("https://api.github.com/", kAnchors, true), nullptr);
  EXPECT_EQ(verifiedFetchRefusal("HTTPS://api.github.com/", kAnchors, true), nullptr);
}

// wolfSSL has no default bundle, so proceeding here would mean setInsecure():
// exactly the silent downgrade the verified entry point exists to rule out.
TEST(VerifiedFetchRefusal, WithAnchorRequirementNullAnchorsAreRefused) {
  EXPECT_NE(verifiedFetchRefusal("https://api.github.com/", nullptr, true), nullptr);
}

TEST(VerifiedFetchRefusal, WithAnchorRequirementEmptyAnchorsAreRefused) {
  EXPECT_NE(verifiedFetchRefusal("https://api.github.com/", "", true), nullptr);
}

TEST(VerifiedFetchRefusal, SchemeIsCheckedBeforeAnchors) {
  // Both preconditions fail; the reported reason is the transport one.
  EXPECT_STREQ(verifiedFetchRefusal("http://api.github.com/", nullptr, true), "URL is not https");
}

// The build-derived value the production call site passes for needsAnchors.
TEST(VerifiedFetchNeedsAnchors, TracksTheTlsBackend) {
#if defined(FREEINK_NET_WOLFSSL)
  EXPECT_TRUE(kVerifiedFetchNeedsAnchors);
  EXPECT_NE(verifiedFetchRefusal("https://api.github.com/", nullptr, kVerifiedFetchNeedsAnchors), nullptr);
#else
  EXPECT_FALSE(kVerifiedFetchNeedsAnchors);
  EXPECT_EQ(verifiedFetchRefusal("https://api.github.com/", nullptr, kVerifiedFetchNeedsAnchors), nullptr);
#endif
  // Plaintext is refused whichever backend this translation unit is built for.
  EXPECT_NE(verifiedFetchRefusal("http://api.github.com/", kAnchors, kVerifiedFetchNeedsAnchors), nullptr);
}

}  // namespace
