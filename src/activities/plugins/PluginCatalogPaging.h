#pragma once

#include <cstddef>
#include <string_view>

namespace plugincatalogpaging {

// A server-provided page template returns only the requested slice. A static
// feed contains all rows and must be advanced locally during streaming.
inline bool isStaticFeed(const std::string_view url, const std::string_view body) {
  return url.find("{page}") == std::string_view::npos && body.find("{page}") == std::string_view::npos;
}

// Counts eligible rows without retaining them. On a static feed the window
// starts at (page - 1) * pageSize; for server-paged feeds it starts at zero.
// The one-row lookahead determines whether Next Page is real.
class PageWindow {
 public:
  PageWindow(const size_t start, const size_t limit) : start_(start), limit_(limit) {}

  bool take(const bool eligible = true) {
    if (!eligible) return false;
    const size_t index = seen_++;
    if (index < start_) return false;
    if (index - start_ < limit_) return true;
    hasMore_ = true;
    return false;
  }

  bool hasMore() const { return hasMore_; }

 private:
  size_t start_;
  size_t limit_;
  size_t seen_ = 0;
  bool hasMore_ = false;
};
}  // namespace plugincatalogpaging
