#include "OpdsParser.h"

#include <Logging.h>
#include <XmlParserUtils.h>

#include <cctype>
#include <cstring>
#include <string_view>
#include <utility>

namespace {
constexpr size_t ENTRY_STORAGE_CAPACITY = 64;
constexpr size_t MAX_ENTRIES = ENTRY_STORAGE_CAPACITY - 2;
constexpr size_t MAX_TITLE_CHARS = 160;
constexpr size_t MAX_AUTHOR_CHARS = 120;
constexpr size_t MAX_ID_CHARS = 128;
constexpr size_t MAX_HREF_CHARS = 768;
constexpr size_t MAX_SEARCH_TEMPLATE_CHARS = 768;
constexpr size_t MAX_PAGE_URL_CHARS = 768;
constexpr size_t MAX_ACQUISITION_LINKS = 3;
constexpr size_t MAX_ACQUISITION_HREF_CHARS = MAX_ENTRIES * MAX_HREF_CHARS;

std::string_view mimeTypeToken(const char* type) {
  if (!type) return {};

  std::string_view token{type};
  if (const size_t parameters = token.find(';'); parameters != std::string_view::npos) {
    token = token.substr(0, parameters);
  }
  while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) token.remove_prefix(1);
  while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) token.remove_suffix(1);
  return token;
}

bool equalsIgnoreCase(const std::string_view value, const std::string_view expected) {
  if (value.size() != expected.size()) return false;
  for (size_t i = 0; i < value.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(expected[i]))) {
      return false;
    }
  }
  return true;
}

OpdsAcquisitionFormat formatFromMimeType(const std::string_view type) {
  if (equalsIgnoreCase(type, "application/epub+zip")) return OpdsAcquisitionFormat::EPUB;
  if (equalsIgnoreCase(type, "application/x-xtc+zip") || equalsIgnoreCase(type, "application/vnd.xteink.xtc")) {
    return OpdsAcquisitionFormat::XTC;
  }
  if (equalsIgnoreCase(type, "application/x-xtch+zip") || equalsIgnoreCase(type, "application/vnd.xteink.xtch")) {
    return OpdsAcquisitionFormat::XTCH;
  }
  return OpdsAcquisitionFormat::UNKNOWN;
}

bool hasSuffixIgnoreCase(const std::string_view value, const std::string_view suffix) {
  return value.size() >= suffix.size() && equalsIgnoreCase(value.substr(value.size() - suffix.size()), suffix);
}

OpdsAcquisitionFormat formatFromUrl(const char* href) {
  std::string_view path{href ? href : ""};
  if (const size_t suffix = path.find_first_of("?#"); suffix != std::string_view::npos) {
    path = path.substr(0, suffix);
  }
  while (!path.empty() && path.back() == '/') path.remove_suffix(1);
  if (hasSuffixIgnoreCase(path, ".xtch")) return OpdsAcquisitionFormat::XTCH;
  if (hasSuffixIgnoreCase(path, ".xtc")) return OpdsAcquisitionFormat::XTC;
  return OpdsAcquisitionFormat::UNKNOWN;
}

OpdsAcquisitionFormat acquisitionFormat(const char* type, const char* href) {
  const std::string_view mimeType = mimeTypeToken(type);
  if (const auto format = formatFromMimeType(mimeType); format != OpdsAcquisitionFormat::UNKNOWN) return format;

  const bool genericMime = mimeType.empty() || equalsIgnoreCase(mimeType, "application/octet-stream") ||
                           equalsIgnoreCase(mimeType, "binary/octet-stream");
  return genericMime ? formatFromUrl(href) : OpdsAcquisitionFormat::UNKNOWN;
}
}  // namespace

const char* opdsAcquisitionExtension(const OpdsAcquisitionFormat format) {
  switch (format) {
    case OpdsAcquisitionFormat::EPUB:
      return ".epub";
    case OpdsAcquisitionFormat::XTC:
      return ".xtc";
    case OpdsAcquisitionFormat::XTCH:
      return ".xtch";
    case OpdsAcquisitionFormat::UNKNOWN:
    default:
      return "";
  }
}

const char* opdsAcquisitionLabel(const OpdsAcquisitionFormat format) {
  switch (format) {
    case OpdsAcquisitionFormat::EPUB:
      return "EPUB";
    case OpdsAcquisitionFormat::XTC:
      return "XTC";
    case OpdsAcquisitionFormat::XTCH:
      return "XTCH";
    case OpdsAcquisitionFormat::UNKNOWN:
    default:
      return "";
  }
}

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
  nextPageUrl.clear();
  prevPageUrl.clear();
  currentEntry = OpdsEntry{};
  currentText.clear();
  acquisitionHrefChars = 0;
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
    return;
  }

  if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
    const char* href = findAttribute(atts, "href");
    if (href && href[0] != '\0') {
      const char* rel = findAttribute(atts, "rel");
      const char* type = findAttribute(atts, "type");

      if (rel && strcmp(rel, "search") == 0) {
        if (strstr(href, "{searchTerms}") != nullptr) {
          assignBounded(self->searchTemplate, href, MAX_SEARCH_TEMPLATE_CHARS);
        }
      } else if (rel && strcmp(rel, "next") == 0 && !self->inEntry) {
        assignBounded(self->nextPageUrl, href, MAX_PAGE_URL_CHARS);
      } else if (rel && strcmp(rel, "previous") == 0 && !self->inEntry) {
        assignBounded(self->prevPageUrl, href, MAX_PAGE_URL_CHARS);
      }

      if (self->inEntry && self->collectCurrentEntry) {
        if (rel && strstr(rel, "opds-spec.org/acquisition") != nullptr) {
          const auto format = acquisitionFormat(type, href);
          const auto& links = self->currentEntry.acquisitionLinks;
          bool alreadyHasFormat = false;
          for (const auto& link : links) {
            if (link.format == format) {
              alreadyHasFormat = true;
              break;
            }
          }

          if (format != OpdsAcquisitionFormat::UNKNOWN && !alreadyHasFormat && links.size() < MAX_ACQUISITION_LINKS) {
            const size_t hrefChars = strnlen(href, MAX_HREF_CHARS);
            if (hrefChars <= MAX_ACQUISITION_HREF_CHARS - self->acquisitionHrefChars) {
              OpdsAcquisitionLink link;
              link.format = format;
              link.href.assign(href, hrefChars);
              self->acquisitionHrefChars += hrefChars;
              self->currentEntry.acquisitionLinks.push_back(std::move(link));
              self->currentEntry.type = OpdsEntryType::BOOK;
              self->currentEntry.href.clear();
            } else {
              self->feedTruncated = true;
            }
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

  if (!self->inEntry || !self->collectCurrentEntry) return;

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
    const bool hasTarget = self->currentEntry.type == OpdsEntryType::BOOK ? !self->currentEntry.acquisitionLinks.empty()
                                                                          : !self->currentEntry.href.empty();
    const bool storeEntry = self->collectCurrentEntry && !self->currentEntry.title.empty() && hasTarget;
    if (storeEntry) {
      self->entries.push_back(std::move(self->currentEntry));
    } else {
      for (const auto& link : self->currentEntry.acquisitionLinks) {
        self->acquisitionHrefChars -= link.href.size();
      }
    }
    self->inEntry = false;
    self->collectCurrentEntry = false;
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
  if (!self->collectCurrentEntry) return;
  if (self->inTitle) {
    appendBounded(self->currentText, s, len, MAX_TITLE_CHARS);
  } else if (self->inAuthorName) {
    appendBounded(self->currentText, s, len, MAX_AUTHOR_CHARS);
  } else if (self->inId) {
    appendBounded(self->currentText, s, len, MAX_ID_CHARS);
  }
}
