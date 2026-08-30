#pragma once
#include <cstddef>
#include <string_view>

// Preconditions for HttpDownloader::fetchUrlVerified(). Header-only and free of
// Arduino/ESP-IDF includes so test/http_verified_fetch can exercise them on the
// host. Nothing here allocates or logs -- the caller turns a refusal reason
// into LOG_ERR.

// True only when the URL's scheme is https, compared case-insensitively: URL
// schemes are case-insensitive (RFC 3986 section 3.1) and esp_http_client
// parses them that way, so a byte-exact "https://" prefix test would refuse a
// perfectly valid HTTPS:// URL while claiming to enforce transport security.
inline bool urlIsHttps(std::string_view url) {
  constexpr std::string_view scheme = "https://";
  if (url.size() < scheme.size()) return false;
  for (size_t i = 0; i < scheme.size(); ++i) {
    char c = url[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != scheme[i]) return false;
  }
  return true;
}

// wolfSSL ships no default trust store, so a verified fetch over that transport
// is only meaningful with caller-supplied anchors. The esp_http_client path
// verifies against the compiled-in esp_crt_bundle instead and needs none.
#if defined(FREEINK_NET_WOLFSSL)
inline constexpr bool kVerifiedFetchNeedsAnchors = true;
#else
inline constexpr bool kVerifiedFetchNeedsAnchors = false;
#endif

// nullptr when a verified fetch of url may proceed, otherwise a flash-resident
// reason string for the caller to log. Fails closed on both counts: "verified"
// means TLS as well as chain checking, so plaintext is refused here rather than
// trusting every call site to only ever pass an https URL; and where the build
// has no default trust store, a missing or empty anchor set is refused rather
// than falling through to setInsecure() -- the exact silent downgrade
// fetchUrlVerified exists to rule out.
inline const char* verifiedFetchRefusal(std::string_view url, const char* caPem, bool needsAnchors) {
  if (!urlIsHttps(url)) return "URL is not https";
  if (needsAnchors && (caPem == nullptr || *caPem == '\0')) return "no CA anchors supplied";
  return nullptr;
}
