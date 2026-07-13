#pragma once
#include <expat.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct WebDavEntry {
  std::string name;
  std::string href;
  size_t contentLength = 0;
  bool isCollection = false;
};

class WebDavParser {
 public:
  WebDavParser();
  ~WebDavParser();

  WebDavParser(const WebDavParser&) = delete;
  WebDavParser& operator=(const WebDavParser&) = delete;

  size_t write(uint8_t c);
  size_t write(const uint8_t* data, size_t len);
  void flush();

  bool error() const { return errorOccurred; }

  const std::vector<WebDavEntry>& getEntries() const& { return entries; }
  std::vector<WebDavEntry> getEntries() && { return std::move(entries); }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);

  static bool endsWith(const char* str, const char* suffix);

  XML_Parser parser = nullptr;
  std::vector<WebDavEntry> entries;
  WebDavEntry currentEntry;
  std::string currentText;

  bool inResponse = false;
  bool inHref = false;
  bool inDisplayName = false;
  bool inContentLength = false;
  bool inCollection = false;

  bool errorOccurred = false;
};

namespace WebDavClient {

bool listFiles(const char* url, const char* username, const char* password, std::vector<WebDavEntry>& entries);

}
