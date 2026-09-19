#pragma once
#include <cstddef>
#include <cstring>
#include <string>

/**
 * Type of OPDS entry.
 */
enum class OpdsEntryType {
  NAVIGATION,  // Link to another catalog
  BOOK         // Downloadable book
};

/**
 * Represents an entry from an OPDS feed (either a navigation link or a book).
 * Shared between the OPDS 1.x (Atom) and OPDS 2.0 (JSON) parsers.
 */
struct OpdsEntry {
  OpdsEntryType type = OpdsEntryType::NAVIGATION;
  std::string title;
  std::string author;  // Only for books
  std::string href;    // Navigation URL or epub download URL
  std::string id;
  // Section heading drawn above this row (group or facet-group title).
  std::string heading;
  // Right-column annotation (e.g. a facet's publication count).
  std::string detail;
};

// Entry id marking a group's "see all" link; the UI supplies the label.
inline constexpr const char* OPDS_SEE_ALL_ID = "opds:group-self";

/**
 * Preference rank of an acquisition link relation, covering both the OPDS 1.x
 * URI forms and the OPDS 2.0 short names. Higher ranks are preferred when a
 * publication offers several acquisition links. -1 means the link is not a
 * usable acquisition for this device: not an acquisition rel at all, `buy`
 * (needs a payment flow), or `sample`/`preview` (not the full book).
 */
inline int opdsAcquisitionRank(const char* rel) {
  if (strstr(rel, "opds-spec.org/acquisition") != nullptr) {
    if (strstr(rel, "/open-access") != nullptr) return 3;
    if (strstr(rel, "/sample") != nullptr || strstr(rel, "/buy") != nullptr) return -1;
    if (strstr(rel, "/borrow") != nullptr || strstr(rel, "/subscribe") != nullptr) return 1;
    return 2;  // bare http://opds-spec.org/acquisition
  }
  if (strcmp(rel, "download") == 0 || strcmp(rel, "open-access") == 0) return 3;
  if (strcmp(rel, "acquisition") == 0) return 2;
  if (strcmp(rel, "borrow") == 0 || strcmp(rel, "subscribe") == 0) return 1;
  return -1;  // buy, preview, or not an acquisition rel
}

// Shared memory bounds for both feed parsers.
namespace OpdsLimits {
constexpr size_t ENTRY_STORAGE_CAPACITY = 64;
constexpr size_t MAX_ENTRIES = ENTRY_STORAGE_CAPACITY - 2;
constexpr size_t MAX_TITLE_CHARS = 160;
constexpr size_t MAX_AUTHOR_CHARS = 120;
constexpr size_t MAX_ID_CHARS = 128;
constexpr size_t MAX_HREF_CHARS = 768;
constexpr size_t MAX_SEARCH_TEMPLATE_CHARS = 768;
constexpr size_t MAX_PAGE_URL_CHARS = 768;
}  // namespace OpdsLimits
