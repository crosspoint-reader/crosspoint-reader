#pragma once

// What a failed OPDS feed fetch should tell the user. Pure: `status` is the HTTP
// status of the last request, or 0 when it never got a response.
enum class OpdsFeedError {
  Unreachable,   // DNS, TCP, TLS or timeout: no HTTP response at all
  Unauthorized,  // 401: the catalog wants credentials, or different ones
  Redirected,    // 401 from another origin after a redirect, where credentials are withheld
  ServerError,   // 5xx, including Cloudflare's 530 for an origin tunnel that is down
  Other,         // anything else; the generic "failed to fetch"
};

// `redirected`: the answer came from a different origin than the one requested.
constexpr OpdsFeedError classifyFeedFailure(const int status, const bool redirected = false) {
  if (status <= 0) return OpdsFeedError::Unreachable;
  if (status == 401) return redirected ? OpdsFeedError::Redirected : OpdsFeedError::Unauthorized;
  if (status >= 500) return OpdsFeedError::ServerError;
  return OpdsFeedError::Other;
}
