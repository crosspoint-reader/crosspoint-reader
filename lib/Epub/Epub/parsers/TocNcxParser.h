#pragma once
#include <Print.h>
#include <expat.h>

#include <string>
#include <vector>

class BookMetadataCache;

class TocNcxParser final : public Print {
  enum ParserState {
    START,
    IN_NCX,
    IN_NAV_MAP,
    IN_NAV_POINT,
    IN_NAV_LABEL,
    IN_NAV_LABEL_TEXT,
    IN_CONTENT,
    IN_PAGE_LIST,
    IN_PAGE_TARGET,
    IN_PAGE_TARGET_LABEL,
    IN_PAGE_TARGET_LABEL_TEXT,
  };

 public:
  // One printed-page reference from <pageList>: file href (normalised) + anchor fragment + visible label.
  struct PageListEntry {
    std::string href;    // normalised path to spine item
    std::string anchor;  // fragment (empty = top of file)
    std::string label;   // value shown to the reader (e.g. "1", "iv")
  };

 private:
  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;

  std::string currentLabel;
  std::string currentSrc;
  uint8_t currentDepth = 0;

  // <pageList> collection state
  std::string currentPageLabel;
  std::string currentPageSrc;
  std::vector<PageListEntry> pageList;

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  explicit TocNcxParser(const std::string& baseContentPath, const size_t xmlSize, BookMetadataCache* cache)
      : baseContentPath(baseContentPath), remainingSize(xmlSize), cache(cache) {}
  ~TocNcxParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;

  const std::vector<PageListEntry>& getPageList() const { return pageList; }
};
