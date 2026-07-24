#include "BookSearchUtils.h"

#include <Utf8.h>

#include <algorithm>
#include <cctype>

namespace {

uint32_t lowerVietnamese(uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') return cp + ('a' - 'A');
  if (cp >= 0x1EA0 && cp <= 0x1EF8 && (cp & 1u) == 0) return cp + 1;
  switch (cp) {
    case 0x00C0: return 0x00E0;
    case 0x00C1: return 0x00E1;
    case 0x00C2: return 0x00E2;
    case 0x00C3: return 0x00E3;
    case 0x00C8: return 0x00E8;
    case 0x00C9: return 0x00E9;
    case 0x00CA: return 0x00EA;
    case 0x00CC: return 0x00EC;
    case 0x00CD: return 0x00ED;
    case 0x00D2: return 0x00F2;
    case 0x00D3: return 0x00F3;
    case 0x00D4: return 0x00F4;
    case 0x00D5: return 0x00F5;
    case 0x00D9: return 0x00F9;
    case 0x00DA: return 0x00FA;
    case 0x00DD: return 0x00FD;
    case 0x0102: return 0x0103;
    case 0x0110: return 0x0111;
    case 0x0128: return 0x0129;
    case 0x0168: return 0x0169;
    case 0x01A0: return 0x01A1;
    case 0x01AF: return 0x01B0;
    default: return cp;
  }
}

char foldVietnamese(const uint32_t cp) {
  switch (lowerVietnamese(cp)) {
    case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x0103:
    case 0x1EA1: case 0x1EA3: case 0x1EA5: case 0x1EA7: case 0x1EA9: case 0x1EAB:
    case 0x1EAD: case 0x1EAF: case 0x1EB1: case 0x1EB3: case 0x1EB5: case 0x1EB7:
      return 'a';
    case 0x00E8: case 0x00E9: case 0x00EA: case 0x1EB9: case 0x1EBB: case 0x1EBD:
    case 0x1EBF: case 0x1EC1: case 0x1EC3: case 0x1EC5: case 0x1EC7:
      return 'e';
    case 0x00EC: case 0x00ED: case 0x0129: case 0x1EC9: case 0x1ECB:
      return 'i';
    case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x01A1:
    case 0x1ECD: case 0x1ECF: case 0x1ED1: case 0x1ED3: case 0x1ED5: case 0x1ED7:
    case 0x1ED9: case 0x1EDB: case 0x1EDD: case 0x1EDF: case 0x1EE1: case 0x1EE3:
      return 'o';
    case 0x00F9: case 0x00FA: case 0x0169: case 0x01B0: case 0x1EE5: case 0x1EE7:
    case 0x1EE9: case 0x1EEB: case 0x1EED: case 0x1EEF: case 0x1EF1:
      return 'u';
    case 0x00FD: case 0x1EF3: case 0x1EF5: case 0x1EF7: case 0x1EF9:
      return 'y';
    case 0x0111: return 'd';
    default: return 0;
  }
}

bool isSeparator(const uint32_t cp) {
  return cp <= 0x20 || (cp < 0x80 && !std::isalnum(static_cast<unsigned char>(cp))) ||
         (cp >= 0x2000 && cp <= 0x206F);
}

std::string normalize(const std::string_view source, const bool fold, const size_t maxBytes) {
  std::string input(source.substr(0, maxBytes));
  input = utf8ComposeNfc(input);

  std::string out;
  out.reserve(std::min(input.size(), maxBytes));
  bool pendingSpace = false;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(input.c_str());
  while (*p && out.size() < maxBytes) {
    uint32_t cp = lowerVietnamese(utf8NextCodepoint(&p));
    if (cp == REPLACEMENT_GLYPH || isSeparator(cp)) {
      pendingSpace = !out.empty();
      continue;
    }
    if (fold && utf8IsCombiningMark(cp)) continue;

    std::string encoded;
    const char folded = fold ? foldVietnamese(cp) : 0;
    if (folded) {
      encoded.push_back(folded);
    } else {
      utf8AppendCodepoint(cp, encoded);
    }
    const size_t spaceBytes = pendingSpace ? 1 : 0;
    if (out.size() + spaceBytes + encoded.size() > maxBytes) break;
    if (pendingSpace) out.push_back(' ');
    out += encoded;
    pendingSpace = false;
  }
  return out;
}

}  // namespace

BookSearchQuery makeBookSearchQuery(const std::string_view text) {
  return {normalize(text, false, BOOK_SEARCH_QUERY_BYTES), normalize(text, true, BOOK_SEARCH_QUERY_BYTES)};
}

BookSearchMatch matchBookSearch(const BookSearchQuery& query, const std::string_view candidate) {
  if (query.empty()) return BookSearchMatch::None;
  const std::string exact = normalize(candidate, false, BOOK_SEARCH_CANDIDATE_BYTES);
  if (exact.find(query.exact) != std::string::npos) return BookSearchMatch::Exact;
  const std::string folded = normalize(candidate, true, BOOK_SEARCH_CANDIDATE_BYTES);
  return folded.find(query.folded) != std::string::npos ? BookSearchMatch::Folded : BookSearchMatch::None;
}

void addRankedBookSearchResult(std::vector<size_t>& results, size_t& exactCount, bool& truncated,
                               const size_t sourceIndex, const BookSearchMatch match, const size_t maxResults) {
  if (match == BookSearchMatch::None) return;
  if (maxResults == 0) {
    truncated = true;
    return;
  }
  if (match == BookSearchMatch::Exact) {
    if (exactCount >= maxResults) {
      truncated = true;
      return;
    }
    results.insert(results.begin() + static_cast<std::ptrdiff_t>(exactCount), sourceIndex);
    ++exactCount;
    if (results.size() > maxResults) {
      results.pop_back();
      truncated = true;
    }
    return;
  }
  if (results.size() < maxResults) {
    results.push_back(sourceIndex);
  } else {
    truncated = true;
  }
}
