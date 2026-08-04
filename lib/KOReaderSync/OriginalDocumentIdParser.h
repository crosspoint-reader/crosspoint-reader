#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "expat.h"

/**
 * Streaming OPF parser for the original KOReader document ID written by the
 * browser EPUB optimizer.
 */
class OriginalDocumentIdParser {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
  };

  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void endElement(void* userData, const XML_Char* name);

 public:
  std::string documentId;

  explicit OriginalDocumentIdParser(size_t xmlSize) : remainingSize(xmlSize) {}
  ~OriginalDocumentIdParser();

  bool setup();
  size_t write(const uint8_t* buffer, size_t size);
};
