#pragma once
#include <Print.h>
#include <expat.h>

#include <string>
#include <vector>

#include "OpdsEntry.h"

// Legacy alias for backward compatibility
using OpdsBook = OpdsEntry;

/**
 * Parser for OPDS (Open Publication Distribution System) Atom feeds.
 * Uses the Expat XML parser to parse OPDS catalog entries.
 *
 * Usage:
 *   OpdsParser parser;
 *   if (parser.parse(xmlData, xmlLength)) {
 *     for (const auto& entry : parser.getEntries()) {
 *       if (entry.type == OpdsEntryType::BOOK) {
 *         // Downloadable book
 *       } else {
 *         // Navigation link to another catalog
 *       }
 *     }
 *   }
 */
class OpdsParser final : public Print {
 public:
  OpdsParser();
  ~OpdsParser();

  // Disable copy
  const std::string& getSearchTemplate() const { return searchTemplate; }
  // rel="search" link without an inline template: an OpenSearch description
  // document the caller must fetch and parse to obtain the template.
  const std::string& getSearchDescriptionUrl() const { return searchDescriptionUrl; }
  const std::string& getNextPageUrl() const { return nextPageUrl; }
  const std::string& getPrevPageUrl() const { return prevPageUrl; }
  const std::string& getFirstPageUrl() const { return firstPageUrl; }
  const std::string& getLastPageUrl() const { return lastPageUrl; }
  const std::string& getFeedTitle() const { return feedTitle; }
  // Pagination metadata from the opensearch:* feed elements; 0 when absent.
  uint32_t getNumberOfItems() const { return totalResults; }
  uint32_t getItemsPerPage() const { return itemsPerPage; }
  uint32_t getCurrentPage() const {
    if (itemsPerPage == 0 || startIndex == 0) return 0;
    return (startIndex - 1) / itemsPerPage + 1;
  }
  OpdsParser(const OpdsParser&) = delete;
  OpdsParser& operator=(const OpdsParser&) = delete;

  size_t write(uint8_t) override;
  size_t write(const uint8_t*, size_t) override;

  void flush() override;

  bool error() const;
  bool truncated() const { return feedTruncated; }

  operator bool() { return !error(); }

  /**
   * Get the parsed entries (both navigation and book entries).
   * @return Vector of OpdsEntry entries
   */
  const std::vector<OpdsEntry>& getEntries() const& { return entries; }
  std::vector<OpdsEntry> getEntries() && { return std::move(entries); }

  /**
   * Get only book entries (legacy compatibility).
   * @return Vector of book entries
   */
  std::vector<OpdsEntry> getBooks() const;

  /**
   * Clear all parsed entries.
   */
  void clear();

 private:
  // Expat callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);

  std::string searchTemplate;
  std::string searchDescriptionUrl;
  std::string nextPageUrl;
  std::string prevPageUrl;
  std::string firstPageUrl;
  std::string lastPageUrl;
  std::string feedTitle;
  uint32_t totalResults = 0;
  uint32_t startIndex = 0;
  uint32_t itemsPerPage = 0;
  // Helper to find attribute value
  static const char* findAttribute(const XML_Char** atts, const char* name);
  static void assignBounded(std::string& target, const char* value, size_t maxLen);
  static void appendBounded(std::string& target, const char* value, size_t len, size_t maxLen);

  XML_Parser parser = nullptr;
  std::vector<OpdsEntry> entries;
  OpdsEntry currentEntry;
  std::string currentText;

  // Parser state
  bool inEntry = false;
  bool inFeedTitle = false;
  // Which opensearch:* feed-level counter element is open (else NONE).
  enum class MetaField : uint8_t { NONE, TOTAL_RESULTS, START_INDEX, ITEMS_PER_PAGE } metaField = MetaField::NONE;
  bool inTitle = false;
  bool inAuthor = false;
  bool inAuthorName = false;
  bool inId = false;
  bool collectCurrentEntry = false;
  // Best acquisition rank committed for the current entry, and whether that
  // href points at a plain EPUB (see opdsAcquisitionRank()).
  int entryAcqRank = -1;
  bool entryHasPlainEpub = false;

  bool errorOccured = false;
  bool feedTruncated = false;
};
