#pragma once
#include <expat.h>

#include <string>
#include <vector>

/**
 * Type of OPDS entry.
 */
enum class OpdsEntryType {
  NAVIGATION,  // Link to another catalog
  BOOK         // Downloadable book
};

/**
 * Represents an entry from an OPDS feed (either a navigation link or a book).
 */
struct OpdsEntry {
  OpdsEntryType type = OpdsEntryType::NAVIGATION;
  std::string title;
  std::string author;  // Only for books
  std::string href;    // Navigation URL or epub download URL
  std::string id;
};

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
class OpdsParser {
 public:
  OpdsParser() = default;
  ~OpdsParser();

  // Disable copy
  OpdsParser(const OpdsParser&) = delete;
  OpdsParser& operator=(const OpdsParser&) = delete;

  /**
   * Parse an OPDS XML feed.
   * @param xmlData Pointer to the XML data
   * @param length Length of the XML data
   * @return true if parsing succeeded, false on error
   */
  bool parse(const char* xmlData, size_t length);

  /**
   * Get the parsed entries (both navigation and book entries).
   * @return Vector of OpdsEntry entries
   */
  const std::vector<OpdsEntry>& getEntries() const { return entries; }

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

  // Helper to find attribute value
  static const char* findAttribute(const XML_Char** atts, const char* name);

  XML_Parser parser = nullptr;
  std::vector<OpdsEntry> entries;
  OpdsEntry currentEntry;
  std::string currentText;

  // Parser state
  bool inEntry = false;
  bool inTitle = false;
  bool inAuthor = false;
  bool inAuthorName = false;
  bool inId = false;
};
