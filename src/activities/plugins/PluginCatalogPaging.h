#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace plugincatalogpaging {

// How a JSON browse request pages, from the variables its templates use.
enum class Paging : uint8_t {
  Static,      // no {page} or {offset}: the response holds every row
  Offset,      // {offset}: rows from a position; {limit} adds one lookahead row
  PageNumber,  // {page} without {limit}: the server's own page size
  SizedPages,  // {page} with {limit}: {limit} is the page size, no lookahead row
};

template <typename Headers>
inline Paging pagingOf(const std::string_view url, const std::string_view body, const Headers& headers) {
  const auto uses = [&](const std::string_view key) {
    if (url.find(key) != std::string_view::npos || body.find(key) != std::string_view::npos) return true;
    return std::any_of(headers.begin(), headers.end(), [&](const auto& header) {
      return std::string_view(header.second).find(key) != std::string_view::npos;
    });
  };
  if (uses("{offset}")) return Paging::Offset;
  if (!uses("{page}")) return Paging::Static;
  return uses("{limit}") ? Paging::SizedPages : Paging::PageNumber;
}

// The {limit} value: a page plus one lookahead row, except for sized pages,
// where a server returning page + 1 rows per page would skip one row per page.
inline int limitFor(const Paging paging, const int pageSize) {
  return paging == Paging::SizedPages ? pageSize : pageSize + 1;
}

// Whether a row counts toward the page window. The server counted every row it
// sent, shown or not; a static list counts only the rows it shows.
inline bool countsTowardPage(const Paging paging, const bool shown) { return paging != Paging::Static || shown; }

// Counts eligible rows without retaining them. On a static feed the window
// starts at (page - 1) * pageSize; for server-paged feeds it starts at zero.
// A lookahead row, or a full page for sized pages, shows that Next Page is real.
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

  // Next Page is shown after a lookahead row, or, for sized pages, after a full
  // page: such a server cannot send the extra row.
  bool hasMore(const Paging paging) const {
    return hasMore_ || (paging == Paging::SizedPages && seen_ >= start_ + limit_);
  }

 private:
  size_t start_;
  size_t limit_;
  size_t seen_ = 0;
  bool hasMore_ = false;
};
}  // namespace plugincatalogpaging
