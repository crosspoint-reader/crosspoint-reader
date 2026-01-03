#pragma once
#include <expat.h>

#include <string>
#include <vector>

/**
 * Represents a book entry from an OPDS feed.
 */
struct OpdsBook {
  std::string title;
  std::string author;
  std::string epubUrl;  // Relative URL like "/books/get/epub/3/Calibre_Library"
  std::string id;
};

/**
 * Parser for OPDS (Open Publication Distribution System) Atom feeds.
 * Uses the Expat XML parser to parse OPDS catalog entries.
 *
 * Usage:
 *   OpdsParser parser;
 *   if (parser.parse(xmlData, xmlLength)) {
 *     for (const auto& book : parser.getBooks()) {
 *       // Process book entries
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
   * Get the parsed books.
   * @return Vector of OpdsBook entries
   */
  const std::vector<OpdsBook>& getBooks() const { return books; }

  /**
   * Clear all parsed books.
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
  std::vector<OpdsBook> books;
  OpdsBook currentBook;
  std::string currentText;

  // Parser state
  bool inEntry = false;
  bool inTitle = false;
  bool inAuthor = false;
  bool inAuthorName = false;
  bool inId = false;
};
