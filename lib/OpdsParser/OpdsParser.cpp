#include "OpdsParser.h"

#include <Logging.h>
#include <XmlParserUtils.h>

#include <cstdlib>
#include <cstring>

using namespace OpdsLimits;

OpdsParser::OpdsParser() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    errorOccured = true;
    LOG_DBG("OPDS", "Couldn't allocate memory for parser");
    return;
  }
  entries.reserve(ENTRY_STORAGE_CAPACITY);
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
}

OpdsParser::~OpdsParser() { destroyXmlParser(parser); }

size_t OpdsParser::write(uint8_t c) { return write(&c, 1); }

size_t OpdsParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccured) return length;

  const char* currentPos = reinterpret_cast<const char*>(xmlData);
  size_t remaining = length;
  constexpr size_t chunkSize = 1024;

  while (remaining > 0) {
    const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
    void* const buf = XML_GetBuffer(parser, toRead);
    if (!buf) {
      errorOccured = true;
      LOG_DBG("OPDS", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return length;
    }

    memcpy(buf, currentPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
      errorOccured = true;
      LOG_DBG("OPDS", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return length;
    }
    currentPos += toRead;
    remaining -= toRead;
  }
  return length;
}

void OpdsParser::flush() {
  if (errorOccured || !parser) return;
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    destroyXmlParser(parser);
  }
}

bool OpdsParser::error() const { return errorOccured; }

void OpdsParser::clear() {
  entries.clear();
  searchTemplate.clear();
  searchDescriptionUrl.clear();
  nextPageUrl.clear();
  prevPageUrl.clear();
  firstPageUrl.clear();
  lastPageUrl.clear();
  feedTitle.clear();
  totalResults = startIndex = itemsPerPage = 0;
  metaField = MetaField::NONE;
  inFeedTitle = false;
  entryAcqRank = -1;
  entryHasPlainEpub = false;
  currentEntry = OpdsEntry{};
  currentText.clear();
  inEntry = inTitle = inAuthor = inAuthorName = inId = false;
  collectCurrentEntry = false;
  feedTruncated = false;
}

std::vector<OpdsEntry> OpdsParser::getBooks() const {
  std::vector<OpdsEntry> books;
  for (const auto& entry : entries) {
    if (entry.type == OpdsEntryType::BOOK) books.push_back(entry);
  }
  return books;
}

const char* OpdsParser::findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

void OpdsParser::assignBounded(std::string& target, const char* value, const size_t maxLen) {
  if (!value) {
    target.clear();
    return;
  }
  target.assign(value, strnlen(value, maxLen));
}

void OpdsParser::appendBounded(std::string& target, const char* value, const size_t len, const size_t maxLen) {
  if (target.size() >= maxLen) return;
  const size_t remaining = maxLen - target.size();
  target.append(value, len < remaining ? len : remaining);
}

void XMLCALL OpdsParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    self->inEntry = true;
    self->collectCurrentEntry = self->entries.size() < MAX_ENTRIES;
    self->feedTruncated = self->feedTruncated || !self->collectCurrentEntry;
    self->currentEntry = OpdsEntry{};
    self->currentText.clear();
    self->inTitle = self->inAuthor = self->inAuthorName = self->inId = false;
    self->entryAcqRank = -1;
    self->entryHasPlainEpub = false;
    return;
  }

  if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
    const char* href = findAttribute(atts, "href");
    if (href) {
      const char* rel = findAttribute(atts, "rel");
      const char* type = findAttribute(atts, "type");

      if (rel && strcmp(rel, "search") == 0) {
        if (strstr(href, "{searchTerms}") != nullptr) {
          assignBounded(self->searchTemplate, href, MAX_SEARCH_TEMPLATE_CHARS);
        } else {
          // No inline template: href points at an OpenSearch description
          // document (calibre-web, COPS, Kavita).
          assignBounded(self->searchDescriptionUrl, href, MAX_SEARCH_TEMPLATE_CHARS);
        }
      } else if (rel && strcmp(rel, "next") == 0 && !self->inEntry) {
        assignBounded(self->nextPageUrl, href, MAX_PAGE_URL_CHARS);
      } else if (rel && (strcmp(rel, "previous") == 0 || strcmp(rel, "prev") == 0) && !self->inEntry) {
        assignBounded(self->prevPageUrl, href, MAX_PAGE_URL_CHARS);
      } else if (rel && strcmp(rel, "first") == 0 && !self->inEntry) {
        assignBounded(self->firstPageUrl, href, MAX_PAGE_URL_CHARS);
      } else if (rel && strcmp(rel, "last") == 0 && !self->inEntry) {
        assignBounded(self->lastPageUrl, href, MAX_PAGE_URL_CHARS);
      }

      if (self->inEntry && self->collectCurrentEntry) {
        const int rank = rel ? opdsAcquisitionRank(rel) : -1;
        if (rank >= 0 && type && strcmp(type, "application/epub+zip") == 0) {
          // Prefer higher-ranked acquisitions (open-access over borrow,
          // never buy/sample); at equal rank prefer a plain EPUB path over
          // derived formats.
          const bool isPlainEpub = strstr(href, ".epub") != nullptr || strstr(href, "/epub/") != nullptr;
          if (self->currentEntry.type != OpdsEntryType::BOOK || rank > self->entryAcqRank ||
              (rank == self->entryAcqRank && isPlainEpub && !self->entryHasPlainEpub)) {
            self->currentEntry.type = OpdsEntryType::BOOK;
            assignBounded(self->currentEntry.href, href, MAX_HREF_CHARS);
            self->entryAcqRank = rank;
            self->entryHasPlainEpub = isPlainEpub;
          }
        } else if (type && strstr(type, "application/atom+xml") != nullptr) {
          if (self->currentEntry.type != OpdsEntryType::BOOK) {
            self->currentEntry.type = OpdsEntryType::NAVIGATION;
            assignBounded(self->currentEntry.href, href, MAX_HREF_CHARS);
          }
        }
      }
    }
  }

  if (!self->inEntry) {
    if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
      self->inFeedTitle = true;
      self->currentText.clear();
    } else if (strstr(name, "totalResults") != nullptr) {
      self->metaField = MetaField::TOTAL_RESULTS;
      self->currentText.clear();
    } else if (strstr(name, "startIndex") != nullptr) {
      self->metaField = MetaField::START_INDEX;
      self->currentText.clear();
    } else if (strstr(name, "itemsPerPage") != nullptr) {
      self->metaField = MetaField::ITEMS_PER_PAGE;
      self->currentText.clear();
    }
    return;
  }
  if (!self->collectCurrentEntry) return;

  if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
    self->inTitle = true;
    self->currentText.clear();
  } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
    self->inAuthor = true;
  } else if (self->inAuthor && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
    self->inAuthorName = true;
    self->currentText.clear();
  } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
    self->inId = true;
    self->currentText.clear();
  }
}

void XMLCALL OpdsParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    if (self->collectCurrentEntry && !self->currentEntry.title.empty() && !self->currentEntry.href.empty()) {
      self->entries.push_back(self->currentEntry);
    }
    self->inEntry = false;
    self->collectCurrentEntry = false;
  } else if (!self->inEntry && self->inFeedTitle && (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr)) {
    self->feedTitle = self->currentText;
    self->inFeedTitle = false;
  } else if (!self->inEntry && self->metaField != MetaField::NONE) {
    const uint32_t parsed = static_cast<uint32_t>(strtoul(self->currentText.c_str(), nullptr, 10));
    switch (self->metaField) {
      case MetaField::TOTAL_RESULTS:
        self->totalResults = parsed;
        break;
      case MetaField::START_INDEX:
        self->startIndex = parsed;
        break;
      case MetaField::ITEMS_PER_PAGE:
        self->itemsPerPage = parsed;
        break;
      case MetaField::NONE:
        break;
    }
    self->metaField = MetaField::NONE;
  } else if (self->inEntry) {
    if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
      if (self->inTitle) self->currentEntry.title = self->currentText;
      self->inTitle = false;
    } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
      self->inAuthor = false;
    } else if (self->inAuthorName && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
      self->currentEntry.author = self->currentText;
      self->inAuthorName = false;
    } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
      if (self->inId) self->currentEntry.id = self->currentText;
      self->inId = false;
    }
  }
}

void XMLCALL OpdsParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<OpdsParser*>(userData);
  if (!self->inEntry) {
    if (self->inFeedTitle) {
      appendBounded(self->currentText, s, len, MAX_TITLE_CHARS);
    } else if (self->metaField != MetaField::NONE) {
      appendBounded(self->currentText, s, len, 16);
    }
    return;
  }
  if (!self->collectCurrentEntry) return;
  if (self->inTitle) {
    appendBounded(self->currentText, s, len, MAX_TITLE_CHARS);
  } else if (self->inAuthorName) {
    appendBounded(self->currentText, s, len, MAX_AUTHOR_CHARS);
  } else if (self->inId) {
    appendBounded(self->currentText, s, len, MAX_ID_CHARS);
  }
}
