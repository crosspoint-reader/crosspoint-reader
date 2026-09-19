#pragma once
#include <Print.h>
#include <StreamingJsonParser.h>

#include <string>
#include <vector>

#include "OpdsEntry.h"

/**
 * Streaming parser for OPDS 2.0 JSON catalog feeds (application/opds+json).
 * Built on StreamingJsonParser (SAX-style), so a feed of any size is parsed
 * without buffering the response body.
 *
 * Extracts, per https://specs.opds.io/opds-2.0:
 *  - `navigation` links (top-level and inside `groups`) as NAVIGATION entries
 *  - `publications` (top-level and inside `groups`) with an EPUB acquisition
 *    link as BOOK entries, preferring open-access/download links and
 *    excluding buy/sample (see opdsAcquisitionRank)
 *  - `groups`: the group title becomes the section heading of its first
 *    entry; a group `self` link is appended as a "see all" NAVIGATION entry
 *    (id OPDS_SEE_ALL_ID; the UI supplies its label)
 *  - `facets` as a separate entry list, one section per facet group, with
 *    `numberOfItems` as the row detail
 *  - feed `metadata`: title and pagination (numberOfItems, itemsPerPage,
 *    currentPage)
 *  - feed `links`: rel "search" (templated URI), "next"/"previous"/"prev",
 *    "first", "last"
 *
 * `images` and unknown extensions are skipped.
 */
class Opds2Parser final : public Print {
 public:
  Opds2Parser();

  Opds2Parser(const Opds2Parser&) = delete;
  Opds2Parser& operator=(const Opds2Parser&) = delete;

  size_t write(uint8_t c) override;
  size_t write(const uint8_t* data, size_t length) override;
  void flush() override;

  bool error() const;
  bool truncated() const { return feedTruncated; }

  const std::vector<OpdsEntry>& getEntries() const& { return entries; }
  std::vector<OpdsEntry> getEntries() && { return std::move(entries); }
  std::vector<OpdsEntry> takeFacetEntries() { return std::move(facetEntries); }
  const std::string& getFeedTitle() const { return feedTitle; }
  const std::string& getSearchTemplate() const { return searchTemplate; }
  const std::string& getNextPageUrl() const { return nextPageUrl; }
  const std::string& getPrevPageUrl() const { return prevPageUrl; }
  const std::string& getFirstPageUrl() const { return firstPageUrl; }
  const std::string& getLastPageUrl() const { return lastPageUrl; }
  // Pagination metadata; 0 when the feed doesn't provide it.
  uint32_t getNumberOfItems() const { return numberOfItems; }
  uint32_t getItemsPerPage() const { return itemsPerPage; }
  uint32_t getCurrentPage() const { return currentPage; }

 private:
  // Semantic role of each open JSON container, decided from the parent scope
  // and the key that introduced it. SKIP swallows entire subtrees (images,
  // metadata we don't consume, unknown extensions).
  enum class Scope : uint8_t {
    FEED,         // root object
    FEED_META,    // feed "metadata" object
    FEED_TITLE,   // localized feed title object
    FEED_LINKS,   // feed "links" array
    FEED_LINK,    // one feed link object
    LINK_REL,     // "rel" array inside any link object
    NAV,          // "navigation" array (feed or group)
    NAV_LINK,     // one navigation link object
    PUBS,         // "publications" array (feed or group)
    PUB,          // one publication object
    PUB_META,     // publication "metadata" object
    PUB_TITLE,    // localized title object ({"en": "..."})
    AUTHOR,       // contributor object ({"name": ...})
    AUTHOR_ARR,   // contributor array (strings and/or objects)
    PUB_LINKS,    // publication "links" array
    PUB_LINK,     // one publication link object
    GROUPS,       // "groups" array
    GROUP,        // one group object
    GROUP_META,   // group "metadata" object
    GROUP_LINKS,  // group "links" array (rel self -> "see all")
    GROUP_LINK,   // one group link object
    FACETS,       // "facets" array
    FACET,        // one facet group object
    FACET_META,   // facet group "metadata" object
    FACET_LINKS,  // facet group "links" array
    FACET_LINK,   // one facet link object
    FACET_PROPS,  // facet link "properties" object (numberOfItems)
    SKIP,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void onContainerStart(bool isObject);
  void onContainerEnd();
  void onStringValue(const char* value, size_t len);
  void onNumberValue(const char* value);

  Scope scopeForChild(Scope parent, bool isObject) const;
  Scope current() const { return depth > 0 ? stack[depth - 1] : Scope::FEED; }

  void resetLink();
  void commitFeedLink();
  void commitPubLink();
  void commitNavLink();
  void commitGroupLink();
  void commitFacetLink();
  void commitPublication();
  void beginGroup();
  void endGroup();
  void applyRel(const char* rel);

  static void assignBounded(std::string& target, const char* value, size_t len, size_t maxLen);

  StreamingJsonParser parser;

  static constexpr uint8_t MAX_DEPTH = StreamingJsonParser::MAX_NESTING + 1;
  static constexpr size_t MAX_FACET_ENTRIES = 24;
  Scope stack[MAX_DEPTH];
  uint8_t depth = 0;
  bool sawRoot = false;
  bool errorOccured = false;
  bool feedTruncated = false;

  char pendingKey[24] = {0};

  std::vector<OpdsEntry> entries;
  std::vector<OpdsEntry> facetEntries;
  OpdsEntry currentEntry;

  // Accumulator for the link object currently being parsed (feed, navigation,
  // group, facet or publication link — the scope on commit tells which).
  struct {
    std::string href;
    std::string title;
    bool relSearch = false;
    bool relNext = false;
    bool relPrev = false;
    bool relFirst = false;
    bool relLast = false;
    bool relSelf = false;
    int acqRank = -1;
    bool typeEpub = false;
    bool templated = false;
    int32_t numberOfItems = -1;
  } link;
  // Best acquisition rank already committed for the current publication, and
  // whether that href points at a plain EPUB (see commitPubLink()).
  int pubAcqRank = -1;
  bool pubHasPlainEpub = false;

  // Indices of top-level (non-group) navigation entries, for deduplication
  // against groups at flush().
  std::vector<uint16_t> topNavIndices;
  bool sawGroups = false;
  // Current group accumulation (groups don't nest).
  std::string groupTitle;
  std::string groupSelfHref;
  size_t groupStartIndex = 0;
  bool inGroup = false;
  // Current facet group accumulation.
  std::string facetTitle;
  size_t facetStartIndex = 0;

  std::string feedTitle;
  std::string searchTemplate;
  std::string nextPageUrl;
  std::string prevPageUrl;
  std::string firstPageUrl;
  std::string lastPageUrl;
  uint32_t numberOfItems = 0;
  uint32_t itemsPerPage = 0;
  uint32_t currentPage = 0;
};
