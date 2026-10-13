#include "OpdsParser.h"

#include <FsHelpers.h>
#include <Logging.h>

#include <cstring>
#include <utility>

namespace {
// Returns the length of href after trimming trailing slashes.
size_t trimmedHrefLength(const char* href) {
  size_t len = strlen(href);
  while (len > 0 && href[len - 1] == '/') {
    len--;
  }
  return len;
}

std::string_view trimmedHrefView(const char* href) { return std::string_view{href, trimmedHrefLength(href)}; }

// Returns an OpdsAcquisitionLink if the type and href correspond to a supported
// acquisition format, otherwise returns an empty OpdsAcquisitionLink.
OpdsAcquisitionLink supportedAcquisitionLink(const char* type, const char* href) {
  if (!type || !href || type[0] == '\0' || href[0] == '\0') {
    return {};
  }

  // Some OPDS feeds append a trailing slash to format URLs like
  // `/opds/book/123/kepub/`. Trim it so suffix checks work on the final segment.
  const std::string_view trimmedHref = trimmedHrefView(href);

  if (strcmp(type, "application/epub+zip") == 0) {
    if (FsHelpers::checkFileExtension(trimmedHref, ".kepub.epub")) {
      return {href, type, "kepub", ".kepub.epub"};
    }

    // Calibre-Web-Automated uses trailing path segments like `/kepub/` instead of
    // filename extensions, so match `/kepub` after trimming trailing slashes.
    if (FsHelpers::checkFileExtension(trimmedHref, ".kepub") || FsHelpers::checkFileExtension(trimmedHref, "/kepub")) {
      // Save bare KePub downloads with an `.epub` suffix so the existing ePub
      // reader can open them.
      return {href, type, "kepub", ".kepub.epub"};
    }
    return {href, type, "epub", ".epub"};
  }

  if (strcmp(type, "text/plain") == 0) {
    return {href, type, "txt", ".txt"};
  }

  if (strcmp(type, "text/markdown") == 0 || strcmp(type, "text/x-markdown") == 0) {
    return {href, type, "md", ".md"};
  }

  if (FsHelpers::checkFileExtension(trimmedHref, ".xtc")) {
    return {href, "application/vnd.xteink.xtc", "xtc", ".xtc"};
  }

  if (FsHelpers::checkFileExtension(trimmedHref, ".xth") || FsHelpers::checkFileExtension(trimmedHref, ".xtch")) {
    return {href, "application/vnd.xteink.xtch", "xtch", ".xtch"};
  }

  return {};
}

// Determine if the given OpdsEntry's acquisition link href is already present,
// to prevent duplicate download targets.
bool hasEquivalentAcquisitionLink(const OpdsEntry& entry, const OpdsAcquisitionLink& candidate) {
  const std::string_view normalizedCandidateHref = trimmedHrefView(candidate.href.c_str());
  for (const auto& link : entry.acquisitionLinks) {
    if (trimmedHrefView(link.href.c_str()) == normalizedCandidateHref) {
      return true;
    }
  }

  return false;
}
}  // namespace

OpdsParser::OpdsParser() {
  if (!saxParser_.init(this, startElement, endElement, characterData)) {
    errorOccured = true;
    LOG_DBG("OPDS", "Couldn't allocate memory for parser");
  }
}

OpdsParser::~OpdsParser() = default;

size_t OpdsParser::write(uint8_t c) { return write(&c, 1); }

size_t OpdsParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccured) return length;

  if (!saxParser_.feed(xmlData, length)) {
    errorOccured = true;
  }
  return length;
}

void OpdsParser::flush() {
  if (errorOccured) return;
  if (!saxParser_.finalize()) {
    errorOccured = true;
  }
}

bool OpdsParser::error() const { return errorOccured; }

void OpdsParser::clear() {
  searchTemplate.clear();
  osdUrl.clear();
  nextPageUrl.clear();
  prevPageUrl.clear();
  currentEntry = OpdsEntry{};
  currentText.clear();
  inEntry = inTitle = inAuthor = inAuthorName = inId = inSummary = inContent = false;
}

const char* OpdsParser::findAttribute(const char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

void OpdsParser::startElement(void* userData, const char* name, const char** atts) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
    const char* href = findAttribute(atts, "href");
    if (href) {
      const char* rel = findAttribute(atts, "rel");
      const char* type = findAttribute(atts, "type");

      if (rel && strcmp(rel, "search") == 0) {
        std::string sHref(href);
        if (sHref.find("{searchTerms}") != std::string::npos) {
          self->searchTemplate = sHref;
        } else if (type && strcmp(type, "application/opensearchdescription+xml") == 0) {
          self->osdUrl = sHref;
        }
      } else if (rel && strcmp(rel, "next") == 0 && !self->inEntry) {
        self->nextPageUrl = href;
      } else if (rel && strcmp(rel, "previous") == 0 && !self->inEntry) {
        self->prevPageUrl = href;
      }

      if (self->inEntry) {
        if (rel && strstr(rel, "opds-spec.org/acquisition") != nullptr) {
          const auto acquisition = supportedAcquisitionLink(type, href);
          if (!acquisition.formatKey.empty() && !hasEquivalentAcquisitionLink(self->currentEntry, acquisition)) {
            self->currentEntry.type = OpdsEntryType::BOOK;
            if (self->currentEntry.acquisitionLinks.empty()) {
              self->currentEntry.href = href;
            } else if (self->currentEntry.acquisitionLinks.size() == 1 &&
                       self->currentEntry.acquisitionLinks.capacity() < 3) {
              self->currentEntry.acquisitionLinks.reserve(3);
            }
            self->currentEntry.acquisitionLinks.push_back(acquisition);
          }
        } else if (rel && type && strstr(rel, "opds-spec.org/image") != nullptr &&
                   strstr(rel, "thumbnail") == nullptr && strncmp(type, "image/", 6) == 0 &&
                   self->currentEntry.imageHref.empty()) {
          self->currentEntry.imageHref = href;
        } else if (type && strstr(type, "application/atom+xml") != nullptr) {
          if (self->currentEntry.type != OpdsEntryType::BOOK) {
            self->currentEntry.type = OpdsEntryType::NAVIGATION;
            self->currentEntry.href = href;
          }
        }
      }
    }
  }

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    self->inEntry = true;
    self->currentEntry = OpdsEntry{};
    return;
  }

  if (!self->inEntry) return;

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
  } else if (strcmp(name, "summary") == 0 || strstr(name, ":summary") != nullptr) {
    self->inSummary = true;
    self->currentText.clear();
  } else if (strcmp(name, "content") == 0 || strstr(name, ":content") != nullptr) {
    self->inContent = true;
    self->currentText.clear();
  }
}

void OpdsParser::endElement(void* userData, const char* name) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    if (!self->currentEntry.title.empty() && !self->currentEntry.href.empty()) {
      if (self->onEntryParsed) {
        self->onEntryParsed(std::move(self->currentEntry));
        self->currentEntry = OpdsEntry{};
      }
    }
    self->inEntry = false;
  } else if (self->inEntry) {
    if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
      if (self->inTitle) self->currentEntry.title = self->currentText;
      self->inTitle = false;
    } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
      self->inAuthor = false;
    } else if (self->inAuthorName && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
      // The first <author> is the book's author; catalogs list a translator or illustrator after it.
      // From CrossInk commit 234675e5c ("keep first OPDS author", Julia Nguyen).
      if (self->currentEntry.author.empty()) self->currentEntry.author = self->currentText;
      self->inAuthorName = false;
    } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
      if (self->inId) self->currentEntry.id = self->currentText;
      self->inId = false;
    } else if (strcmp(name, "summary") == 0 || strstr(name, ":summary") != nullptr) {
      // <summary> wins over <content>: first non-empty element encountered is kept.
      if (self->inSummary && self->currentEntry.summary.empty()) {
        self->currentEntry.summary = self->currentText;
      }
      self->inSummary = false;
    } else if (strcmp(name, "content") == 0 || strstr(name, ":content") != nullptr) {
      // <content> is a fallback when no <summary> was present in this entry.
      if (self->inContent && self->currentEntry.summary.empty()) {
        self->currentEntry.summary = self->currentText;
      }
      self->inContent = false;
    }
  }
}

void OpdsParser::characterData(void* userData, const char* s, const int len) {
  auto* self = static_cast<OpdsParser*>(userData);
  // Hard caps prevent a malicious or pathological feed from exhausting heap.
  // Summaries/content get a larger budget than short metadata fields.
  if (self->inSummary || self->inContent) {
    constexpr size_t kSummaryMax = 2048;
    const size_t space = kSummaryMax > self->currentText.size() ? kSummaryMax - self->currentText.size() : 0;
    if (space > 0) self->currentText.append(s, std::min<size_t>(len, space));
  } else if (self->inTitle || self->inAuthorName || self->inId) {
    constexpr size_t kFieldMax = 512;
    const size_t space = kFieldMax > self->currentText.size() ? kFieldMax - self->currentText.size() : 0;
    if (space > 0) self->currentText.append(s, std::min<size_t>(len, space));
  }
}
