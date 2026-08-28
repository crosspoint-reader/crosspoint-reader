#pragma once
#include <string_view>

// scheme://host[:port] prefix, i.e. everything before the first '/', '?', or
// '#' after the scheme separator (a URL with no path but a query/fragment --
// e.g. "https://host?key=value" -- has no '/' at all, so stopping at '/'
// alone would fold the query string into the "origin" and misclassify a
// same-origin redirect as cross-origin). Useful for deciding whether a
// redirect hop in HttpDownloader.cpp's runGet()/runGetWolf() crosses trust
// boundaries, e.g. before forwarding an Authorization header to a redirect
// target.
//
// A view into the caller's own buffer -- allocates nothing. Deliberately so:
// this can run on every redirect hop, including ones immediately before a
// TLS handshake that needs a large contiguous heap block for RSA signature
// verification, and an extra allocation in that exact window is itself a
// fragmentation risk on RAM-constrained targets. Never pass the result
// across a C API boundary -- a std::string_view is not null-terminated, so
// callers that need a C string must convert explicitly first; every call
// site in this codebase only ever compares two of these with `==`.
inline std::string_view urlOrigin(std::string_view url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string_view::npos) return url;
  const size_t pathStart = url.find_first_of("/?#", schemeEnd + 3);
  return pathStart == std::string_view::npos ? url : url.substr(0, pathStart);
}
