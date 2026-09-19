#pragma once
#include <Print.h>
#include <expat.h>

#include <string>

/**
 * Minimal parser for OpenSearch description documents
 * (application/opensearchdescription+xml), which OPDS 1.x servers such as
 * calibre-web, COPS and Kavita link from rel="search" instead of inlining a
 * template. Extracts the search URL template from <Url> elements, preferring
 * an Atom feed template over other result types.
 */
class OpenSearchDescParser final : public Print {
 public:
  OpenSearchDescParser();
  ~OpenSearchDescParser();

  OpenSearchDescParser(const OpenSearchDescParser&) = delete;
  OpenSearchDescParser& operator=(const OpenSearchDescParser&) = delete;

  size_t write(uint8_t c) override;
  size_t write(const uint8_t* data, size_t length) override;
  void flush() override;

  bool error() const { return errorOccured; }

  // Search URL template containing {searchTerms}; empty if none was found.
  const std::string& getTemplate() const { return searchTemplate; }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);

  XML_Parser parser = nullptr;
  std::string searchTemplate;
  bool templateIsAtom = false;
  bool errorOccured = false;
};
