#pragma once
#include <Print.h>
#include <expat.h>

#include <string>
#include <vector>

// Parser for EPUB 2.01 page-map.xml. Each <page name="X" href="...#anchor"/> element
// maps a printed page number to a spine location. Same output shape as TocNcxParser
// and TocNavParser so all three feed the shared pagelist.bin writer.
class PageMapParser final : public Print {
 public:
  struct PageListEntry {
    std::string href;
    std::string anchor;
    std::string label;
  };

 private:
  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  std::vector<PageListEntry> pageList;

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);

 public:
  explicit PageMapParser(const std::string& baseContentPath, const size_t xmlSize)
      : baseContentPath(baseContentPath), remainingSize(xmlSize) {}
  ~PageMapParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;

  const std::vector<PageListEntry>& getPageList() const { return pageList; }
};
