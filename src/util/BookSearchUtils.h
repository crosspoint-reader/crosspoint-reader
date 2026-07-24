#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class BookSearchMatch : uint8_t { None = 0, Folded = 1, Exact = 2 };

struct BookSearchQuery {
  std::string exact;
  std::string folded;

  bool empty() const { return exact.empty() || folded.empty(); }
};

BookSearchQuery makeBookSearchQuery(std::string_view text);
BookSearchMatch matchBookSearch(const BookSearchQuery& query, std::string_view candidate);
void addRankedBookSearchResult(std::vector<size_t>& results, size_t& exactCount, bool& truncated,
                               size_t sourceIndex, BookSearchMatch match, size_t maxResults);

constexpr size_t BOOK_SEARCH_QUERY_BYTES = 64;
constexpr size_t BOOK_SEARCH_CANDIDATE_BYTES = 512;
// Search screens clamp their layout-derived capacity to this defensive bound.
constexpr size_t BOOK_SEARCH_RESULT_HARD_LIMIT = 32;
