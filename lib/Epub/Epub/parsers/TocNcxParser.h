#pragma once
#include <Print.h>

#include <string>
#include <vector>

#include "Epub/EpubTocEntry.h"
#include "expat.h"

class TocNcxParser final : public Print {
  enum ParserState { START, IN_NCX, IN_NAV_MAP, IN_NAV_POINT, IN_NAV_LABEL, IN_NAV_LABEL_TEXT, IN_CONTENT };

  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;

  std::string currentLabel;
  std::string currentSrc;
  uint8_t currentDepth = 0;

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  std::vector<EpubTocEntry> toc;

  explicit TocNcxParser(const std::string& baseContentPath, const size_t xmlSize)
      : baseContentPath(baseContentPath), remainingSize(xmlSize) {}
  ~TocNcxParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
