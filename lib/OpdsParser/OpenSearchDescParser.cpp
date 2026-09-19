#include "OpenSearchDescParser.h"

#include <Logging.h>
#include <XmlParserUtils.h>

#include <cstring>

#include "OpdsEntry.h"

OpenSearchDescParser::OpenSearchDescParser() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    errorOccured = true;
    LOG_DBG("OPDS", "Couldn't allocate memory for OpenSearch parser");
    return;
  }
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, nullptr);
}

OpenSearchDescParser::~OpenSearchDescParser() { destroyXmlParser(parser); }

size_t OpenSearchDescParser::write(const uint8_t c) { return write(&c, 1); }

size_t OpenSearchDescParser::write(const uint8_t* data, const size_t length) {
  if (errorOccured) return length;
  if (XML_Parse(parser, reinterpret_cast<const char*>(data), static_cast<int>(length), 0) == XML_STATUS_ERROR) {
    errorOccured = true;
    LOG_DBG("OPDS", "OpenSearch parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
    destroyXmlParser(parser);
  }
  return length;
}

void OpenSearchDescParser::flush() {
  if (errorOccured || !parser) return;
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    LOG_DBG("OPDS", "OpenSearch finalization error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
    destroyXmlParser(parser);
  }
}

void XMLCALL OpenSearchDescParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<OpenSearchDescParser*>(userData);
  if (!xmlLocalNameEquals(name, "Url")) return;

  const char* templateAttr = nullptr;
  const char* typeAttr = nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], "template") == 0) {
      templateAttr = atts[i + 1];
    } else if (strcmp(atts[i], "type") == 0) {
      typeAttr = atts[i + 1];
    }
  }
  if (!templateAttr || strstr(templateAttr, "{searchTerms}") == nullptr) return;
  if (strnlen(templateAttr, OpdsLimits::MAX_SEARCH_TEMPLATE_CHARS + 1) > OpdsLimits::MAX_SEARCH_TEMPLATE_CHARS) return;

  const bool isAtom = typeAttr && strstr(typeAttr, "atom+xml") != nullptr;
  // Keep the first template found, but let an Atom-typed one supersede a
  // generic (e.g. text/html) match.
  if (self->searchTemplate.empty() || (isAtom && !self->templateIsAtom)) {
    self->searchTemplate = templateAttr;
    self->templateIsAtom = isAtom;
  }
}
