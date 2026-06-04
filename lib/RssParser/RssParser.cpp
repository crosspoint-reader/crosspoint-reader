#include "RssParser.h"

#include <XmlParserUtils.h>

#ifdef ARDUINO
#include <Logging.h>
#else
#define LOG_DBG(origin, format, ...)
#endif

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {
std::string trim(const std::string& value) {
  size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) start++;
  size_t end = value.size();
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) end--;
  return value.substr(start, end - start);
}

void replaceAll(std::string& value, const char* from, const char* to) {
  size_t pos = 0;
  const size_t fromLen = strlen(from);
  while ((pos = value.find(from, pos)) != std::string::npos) {
    value.replace(pos, fromLen, to);
    pos += strlen(to);
  }
}
}  // namespace

RssParser::RssParser() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    errorOccured = true;
    LOG_DBG("RSS", "Couldn't allocate memory for parser");
  }
}

RssParser::~RssParser() { destroyXmlParser(parser); }

size_t RssParser::write(uint8_t c) { return write(&c, 1); }

size_t RssParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccured) return length;

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  const char* currentPos = reinterpret_cast<const char*>(xmlData);
  size_t remaining = length;
  constexpr size_t chunkSize = 1024;

  while (remaining > 0) {
    const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
    void* const buf = XML_GetBuffer(parser, toRead);
    if (!buf) {
      errorOccured = true;
      LOG_DBG("RSS", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return length;
    }

    memcpy(buf, currentPos, toRead);
    if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
      errorOccured = true;
      LOG_DBG("RSS", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return length;
    }

    currentPos += toRead;
    remaining -= toRead;
  }
  return length;
}

void RssParser::flush() {
  if (!parser) return;
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    destroyXmlParser(parser);
  }
}

void RssParser::clear() {
  feedTitle.clear();
  items.clear();
  currentItem = RssItem{};
  currentText.clear();
  currentElement.clear();
  inChannel = inItem = inFeedTitle = inTextElement = false;
}

bool RssParser::elementMatches(const XML_Char* name, const char* localName) {
  return strcmp(name, localName) == 0 || (strchr(name, ':') && strcmp(strrchr(name, ':') + 1, localName) == 0);
}

const char* RssParser::findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

std::string RssParser::normalizeText(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  bool inTag = false;
  bool lastSpace = false;

  for (size_t i = 0; i < text.size(); i++) {
    const char c = text[i];
    if (c == '<') {
      inTag = true;
      if (!lastSpace) {
        out.push_back(' ');
        lastSpace = true;
      }
      continue;
    }
    if (c == '>') {
      inTag = false;
      continue;
    }
    if (inTag) continue;

    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!lastSpace) {
        out.push_back(' ');
        lastSpace = true;
      }
    } else {
      out.push_back(c);
      lastSpace = false;
    }
  }

  replaceAll(out, "&amp;", "&");
  replaceAll(out, "&lt;", "<");
  replaceAll(out, "&gt;", ">");
  replaceAll(out, "&quot;", "\"");
  replaceAll(out, "&#39;", "'");
  replaceAll(out, "&apos;", "'");
  replaceAll(out, "&nbsp;", " ");
  return trim(out);
}

void XMLCALL RssParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<RssParser*>(userData);

  if (elementMatches(name, "channel") || elementMatches(name, "feed")) {
    self->inChannel = true;
    return;
  }

  if (elementMatches(name, "item") || elementMatches(name, "entry")) {
    self->inItem = true;
    self->currentItem = RssItem{};
    return;
  }

  const bool textElement = elementMatches(name, "title") || elementMatches(name, "link") ||
                           elementMatches(name, "description") || elementMatches(name, "encoded") ||
                           elementMatches(name, "creator") || elementMatches(name, "author") ||
                           elementMatches(name, "pubDate") || elementMatches(name, "updated") ||
                           elementMatches(name, "published") || elementMatches(name, "guid") ||
                           elementMatches(name, "content") || elementMatches(name, "summary");

  if (!textElement) return;

  if (self->inItem && elementMatches(name, "link") && self->currentItem.link.empty()) {
    const char* href = findAttribute(atts, "href");
    if (href) self->currentItem.link = href;
  }

  if (self->inItem || (self->inChannel && elementMatches(name, "title") && self->feedTitle.empty())) {
    self->inTextElement = true;
    self->inFeedTitle = !self->inItem && elementMatches(name, "title");
    self->currentElement = name;
    self->currentText.clear();
  }
}

void XMLCALL RssParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<RssParser*>(userData);

  if ((elementMatches(name, "item") || elementMatches(name, "entry")) && self->inItem) {
    if (!self->currentItem.title.empty() || !self->currentItem.link.empty() || !self->currentItem.content.empty()) {
      if (self->currentItem.title.empty()) self->currentItem.title = self->currentItem.link;
      self->items.push_back(self->currentItem);
    }
    self->inItem = false;
    self->inTextElement = false;
    return;
  }

  if (elementMatches(name, "channel") || elementMatches(name, "feed")) {
    self->inChannel = false;
    return;
  }

  if (!self->inTextElement || !elementMatches(name, strrchr(self->currentElement.c_str(), ':')
                                                       ? strrchr(self->currentElement.c_str(), ':') + 1
                                                       : self->currentElement.c_str())) {
    return;
  }

  const std::string text = normalizeText(self->currentText);
  if (self->inFeedTitle) {
    self->feedTitle = text;
  } else if (self->inItem) {
    if (elementMatches(name, "title")) {
      self->currentItem.title = text;
    } else if (elementMatches(name, "link")) {
      if (!text.empty()) self->currentItem.link = text;
    } else if (elementMatches(name, "encoded") || elementMatches(name, "content")) {
      if (!text.empty()) self->currentItem.content = text;
    } else if (elementMatches(name, "description") || elementMatches(name, "summary")) {
      if (self->currentItem.content.empty()) self->currentItem.content = text;
    } else if (elementMatches(name, "creator") || elementMatches(name, "author")) {
      self->currentItem.author = text;
    } else if (elementMatches(name, "pubDate") || elementMatches(name, "updated") || elementMatches(name, "published")) {
      self->currentItem.published = text;
    } else if (elementMatches(name, "guid")) {
      self->currentItem.guid = text;
    }
  }

  self->inTextElement = false;
  self->inFeedTitle = false;
  self->currentText.clear();
  self->currentElement.clear();
}

void XMLCALL RssParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<RssParser*>(userData);
  if (self->inTextElement) self->currentText.append(s, len);
}
