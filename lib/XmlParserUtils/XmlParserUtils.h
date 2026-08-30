#pragma once

#include <expat.h>

#include <cstring>

// Safely tear down an expat parser: stop processing, clear callbacks, free, and null the pointer.
inline void destroyXmlParser(XML_Parser& parser) {
  if (!parser) return;
  XML_StopParser(parser, XML_FALSE);
  XML_SetElementHandler(parser, nullptr, nullptr);
  XML_SetCharacterDataHandler(parser, nullptr);
  XML_ParserFree(parser);
  parser = nullptr;
}

inline const char* xmlLocalName(const char* qName) {
  if (!qName) return "";
  const char* const separator = std::strchr(qName, ':');
  return separator ? separator + 1 : qName;
}

inline bool xmlLocalNameEquals(const char* qName, const char* expected) {
  return std::strcmp(xmlLocalName(qName), expected) == 0;
}
