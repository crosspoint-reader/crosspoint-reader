#pragma once
#include <Print.h>

#include <map>

#include "Epub.h"
#include "expat.h"

class ContentOpfParser final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_BOOK_TITLE,
    IN_MANIFEST,
    IN_SPINE,
  };

  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  std::string title;
  std::string tocNcxPath;
  std::string coverItemId;
  std::map<std::string, std::string> items;
  std::vector<std::string> spineRefs;

  explicit ContentOpfParser(const std::string& baseContentPath, const size_t xmlSize)
      : baseContentPath(baseContentPath), remainingSize(xmlSize) {}

  bool setup();
  bool teardown();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
