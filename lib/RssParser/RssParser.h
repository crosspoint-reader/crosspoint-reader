#pragma once
#include <Print.h>
#include <expat.h>

#include <string>
#include <vector>

struct RssItem {
  std::string title;
  std::string link;
  std::string author;
  std::string published;
  std::string content;
  std::string guid;
};

class RssParser final : public Print {
 public:
  RssParser();
  ~RssParser();

  RssParser(const RssParser&) = delete;
  RssParser& operator=(const RssParser&) = delete;

  size_t write(uint8_t) override;
  size_t write(const uint8_t*, size_t) override;
  void flush() override;

  bool error() const { return errorOccured; }
  operator bool() const { return !error(); }

  const std::string& getFeedTitle() const { return feedTitle; }
  const std::vector<RssItem>& getItems() const& { return items; }
  std::vector<RssItem> getItems() && { return std::move(items); }

  void clear();

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);

  static bool elementMatches(const XML_Char* name, const char* localName);
  static const char* findAttribute(const XML_Char** atts, const char* name);
  static std::string normalizeText(const std::string& text);

  XML_Parser parser = nullptr;
  std::string feedTitle;
  std::vector<RssItem> items;
  RssItem currentItem;
  std::string currentText;
  std::string currentElement;

  bool inChannel = false;
  bool inItem = false;
  bool inFeedTitle = false;
  bool inTextElement = false;
  bool errorOccured = false;
};
