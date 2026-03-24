#pragma once
#include <expat.h>

#include <Stream.h>
#include <string>
#include <vector>

struct WebDavEntry {
  std::string name;
  std::string href;
  size_t contentLength = 0;
  bool isCollection = false;
};

class WebDavParser final : public Print {
 public:
  WebDavParser();
  ~WebDavParser();

  WebDavParser(const WebDavParser&) = delete;
  WebDavParser& operator=(const WebDavParser&) = delete;

  size_t write(uint8_t) override;
  size_t write(const uint8_t*, size_t) override;
  void flush() override;

  bool error() const { return errorOccurred; }
  operator bool() const { return !errorOccurred; }

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

class WebDavParserStream final : public Stream {
 public:
  explicit WebDavParserStream(WebDavParser& parser) : parser(parser) {}
  ~WebDavParserStream() override { parser.flush(); }

  size_t write(uint8_t c) override { return parser.write(c); }
  size_t write(const uint8_t* buffer, size_t size) override { return parser.write(buffer, size); }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

 private:
  WebDavParser& parser;
};

namespace WebDavClient {

bool listFiles(const char* url, const char* username, const char* password, std::vector<WebDavEntry>& entries);

}
