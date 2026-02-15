#include "OpdsParser.h"

#include <Logging.h>

#include <cstring>

OpdsParser::OpdsParser() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    errorOccured = true;
    LOG_DBG("OPDS", "Couldn't allocate memory for parser");
  }
}

OpdsParser::~OpdsParser() {
  if (parser) {
    XML_StopParser(parser, XML_FALSE);
    XML_SetElementHandler(parser, nullptr, nullptr);
    XML_SetCharacterDataHandler(parser, nullptr);
    XML_ParserFree(parser);
    parser = nullptr;
  }
}

size_t OpdsParser::write(uint8_t c) { return write(&c, 1); }

size_t OpdsParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccured) {
    return length;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  // Parse in chunks to avoid large buffer allocations
  const char* currentPos = reinterpret_cast<const char*>(xmlData);
  size_t remaining = length;
  constexpr size_t chunkSize = 1024;

  while (remaining > 0) {
    void* const buf = XML_GetBuffer(parser, chunkSize);
    if (!buf) {
      errorOccured = true;
      LOG_DBG("OPDS", "Couldn't allocate memory for buffer");
      XML_ParserFree(parser);
      parser = nullptr;
      return length;
    }

    const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
    memcpy(buf, currentPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
      errorOccured = true;
      LOG_DBG("OPDS", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      XML_ParserFree(parser);
      parser = nullptr;
      return length;
    }

    currentPos += toRead;
    remaining -= toRead;
  }
  return length;
}

void OpdsParser::flush() {
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    XML_ParserFree(parser);
    parser = nullptr;
  }
}

bool OpdsParser::error() const { return errorOccured; }

void OpdsParser::clear() {
  entries.clear();
  currentEntry = OpdsEntry{};
  currentText.clear();
  inEntry = false;
  inTitle = false;
  inAuthor = false;
  inAuthorName = false;
  inId = false;
}

std::vector<OpdsEntry> OpdsParser::getBooks() const {
  std::vector<OpdsEntry> books;
  for (const auto& entry : entries) {
    if (entry.type == OpdsEntryType::BOOK) {
      books.push_back(entry);
    }
  }
  return books;
}

const char* OpdsParser::findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) {
      return atts[i + 1];
    }
  }
  return nullptr;
}

void XMLCALL OpdsParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<OpdsParser*>(userData);

  // Check for entry element (with or without namespace prefix)
  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    self->inEntry = true;
    self->currentEntry = OpdsEntry{};
    return;
  }

  if (!self->inEntry) return;

  // Check for title element
  if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
    self->inTitle = true;
    self->currentText.clear();
    return;
  }

  // Check for author element
  if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
    self->inAuthor = true;
    return;
  }

  // Check for author name element
  if (self->inAuthor && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
    self->inAuthorName = true;
    self->currentText.clear();
    return;
  }

  // Check for id element
  if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
    self->inId = true;
    self->currentText.clear();
    return;
  }

  // Check for link element
  if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
    const char* rel = findAttribute(atts, "rel");
    const char* type = findAttribute(atts, "type");
    const char* href = findAttribute(atts, "href");

    if (href) {
      // Check for acquisition link with epub type (this is a downloadable book)
      if (rel && type && strstr(rel, "opds-spec.org/acquisition") != nullptr &&
          strcmp(type, "application/epub+zip") == 0) {
        self->currentEntry.type = OpdsEntryType::BOOK;
        self->currentEntry.href = href;
      }
      // Check for navigation link (subsection or no rel specified with atom+xml type)
      else if (type && strstr(type, "application/atom+xml") != nullptr) {
        // Only set navigation link if we don't already have an epub link
        if (self->currentEntry.type != OpdsEntryType::BOOK) {
          self->currentEntry.type = OpdsEntryType::NAVIGATION;
          self->currentEntry.href = href;
        }
      }
    }
  }
}

void XMLCALL OpdsParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<OpdsParser*>(userData);

  // Check for entry end
  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    // Only add entry if it has required fields (title and href)
    if (!self->currentEntry.title.empty() && !self->currentEntry.href.empty()) {
      self->entries.push_back(self->currentEntry);
    }
    self->inEntry = false;
    self->currentEntry = OpdsEntry{};
    return;
  }

  if (!self->inEntry) return;

  // Check for title end
  if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
    if (self->inTitle) {
      self->currentEntry.title = self->currentText;
    }
    self->inTitle = false;
    return;
  }

  // Check for author end
  if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
    self->inAuthor = false;
    return;
  }

  // Check for author name end
  if (self->inAuthor && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
    if (self->inAuthorName) {
      self->currentEntry.author = self->currentText;
    }
    self->inAuthorName = false;
    return;
  }

  // Check for id end
  if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
    if (self->inId) {
      self->currentEntry.id = self->currentText;
    }
    self->inId = false;
    return;
  }
}

void XMLCALL OpdsParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<OpdsParser*>(userData);

  // Only accumulate text when in a text element
  if (self->inTitle || self->inAuthorName || self->inId) {
    self->currentText.append(s, len);
  }
}
