#include "ContentOpfParser.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Serialization.h>

#include "../BookMetadataCache.h"

namespace {
constexpr char MEDIA_TYPE_NCX[] = "application/x-dtbncx+xml";
constexpr char MEDIA_TYPE_CSS[] = "text/css";
constexpr char itemCacheFile[] = "/.items.bin";
constexpr size_t MAX_DESCRIPTION_LENGTH = 1024;

// Strip HTML tags and collapse whitespace from a description string.
// Expat already decodes XML entities (&lt; → <), so we see raw angle brackets.
std::string stripHtml(const std::string& html) {
  std::string result;
  result.reserve(html.size());
  bool inTag = false;
  for (size_t i = 0; i < html.size(); ++i) {
    const char c = html[i];
    if (c == '<') {
      // Only treat as a tag if immediately followed (no space skip) by a tag-like character
      const size_t j = i + 1;
      if (j < html.size() &&
          (isalpha(static_cast<unsigned char>(html[j])) || html[j] == '/' || html[j] == '!' || html[j] == '?')) {
        inTag = true;
        // Ensure words don't merge when a tag is removed
        if (!result.empty() && result.back() != ' ') result += ' ';
      } else {
        result += c;
      }
    } else if (c == '>') {
      if (inTag) {
        inTag = false;
      } else {
        result += c;
      }
    } else if (!inTag) {
      if (c == '&') {
        // Decode common HTML entities not covered by Expat
        if (html.compare(i, 6, "&nbsp;") == 0) {
          result += ' ';
          i += 5;
        } else if (html.compare(i, 7, "&ndash;") == 0) {
          result += '-';
          i += 6;
        } else if (html.compare(i, 7, "&mdash;") == 0) {
          result += '-';
          i += 6;
        } else if (html.compare(i, 8, "&hellip;") == 0) {
          result += "...";
          i += 7;
        } else
          result += c;
      } else if (c == '\n' || c == '\r' || c == '\t') {
        if (!result.empty() && result.back() != ' ') result += ' ';
      } else {
        result += c;
      }
    }
  }
  // Collapse consecutive spaces and trim trailing whitespace
  std::string out;
  out.reserve(result.size());
  bool lastSpace = false;
  for (char c : result) {
    if (c == ' ') {
      if (!lastSpace && !out.empty()) {
        out += ' ';
        lastSpace = true;
      }
    } else {
      out += c;
      lastSpace = false;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string trim(const std::string& in) {
  size_t start = 0;
  while (start < in.size() && (in[start] == ' ' || in[start] == '\n' || in[start] == '\r' || in[start] == '\t')) {
    ++start;
  }
  size_t end = in.size();
  while (end > start && (in[end - 1] == ' ' || in[end - 1] == '\n' || in[end - 1] == '\r' || in[end - 1] == '\t')) {
    --end;
  }
  return in.substr(start, end - start);
}
}  // namespace

bool ContentOpfParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_DBG("COF", "Couldn't allocate memory for parser");
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

ContentOpfParser::~ContentOpfParser() {
  if (parser) {
    XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
    XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
    XML_SetCharacterDataHandler(parser, nullptr);
    XML_ParserFree(parser);
    parser = nullptr;
  }
  if (tempItemStore) {
    tempItemStore.close();
  }
  const auto itemCachePath = cachePath + itemCacheFile;
  if (Storage.exists(itemCachePath.c_str())) {
    Storage.remove(itemCachePath.c_str());
  }
}

size_t ContentOpfParser::write(const uint8_t data) { return write(&data, 1); }

size_t ContentOpfParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);

    if (!buf) {
      LOG_ERR("COF", "Couldn't allocate memory for buffer");
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      parser = nullptr;
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead) == XML_STATUS_ERROR) {
      LOG_DBG("COF", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      parser = nullptr;
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;
  }

  return size;
}

void XMLCALL ContentOpfParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)atts;

  if (self->state == START && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:title") == 0) {
    // Only capture the first dc:title element; subsequent ones are subtitles
    if (self->title.empty()) {
      self->state = IN_BOOK_TITLE;
    }
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:creator") == 0) {
    self->state = IN_BOOK_AUTHOR;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:language") == 0) {
    self->state = IN_BOOK_LANGUAGE;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:description") == 0) {
    // Only capture the first dc:description element; subsequent ones are alternate/localized variants
    if (self->description.empty()) {
      self->state = IN_BOOK_DESCRIPTION;
    }
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "manifest") == 0 || strcmp(name, "opf:manifest") == 0)) {
    self->state = IN_MANIFEST;
    if (!Storage.openFileForWrite("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for writing. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "spine") == 0 || strcmp(name, "opf:spine") == 0)) {
    self->state = IN_SPINE;
    if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }

    // Sort item index for binary search if we have enough items
    if (self->itemIndex.size() >= LARGE_SPINE_THRESHOLD) {
      std::sort(self->itemIndex.begin(), self->itemIndex.end(), [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
        return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
      });
      self->useItemIndex = true;
      LOG_DBG("COF", "Using fast index for %zu manifest items", self->itemIndex.size());
    }
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "guide") == 0 || strcmp(name, "opf:guide") == 0)) {
    self->state = IN_GUIDE;
    // TODO Remove print
    LOG_DBG("COF", "Entering guide state.");
    if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_METADATA && (strcmp(name, "meta") == 0 || strcmp(name, "opf:meta") == 0)) {
    const char* metaName = nullptr;
    const char* metaContent = nullptr;
    const char* metaProperty = nullptr;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "name") == 0) {
        metaName = atts[i + 1];
      } else if (strcmp(atts[i], "content") == 0) {
        metaContent = atts[i + 1];
      } else if (strcmp(atts[i], "property") == 0) {
        metaProperty = atts[i + 1];
      }
    }

    if (metaName && metaContent) {
      if (strcmp(metaName, "cover") == 0) {
        self->coverItemId = metaContent;
      } else if (strcmp(metaName, "calibre:series") == 0 && self->series.empty()) {
        self->series = trim(std::string(metaContent, std::min(strlen(metaContent), size_t{MAX_DESCRIPTION_LENGTH})));
      } else if (strcmp(metaName, "calibre:series_index") == 0 && self->seriesIndex.empty()) {
        self->seriesIndex =
            trim(std::string(metaContent, std::min(strlen(metaContent), size_t{MAX_DESCRIPTION_LENGTH})));
      }
    }

    // EPUB 3 collection metadata:
    // <meta property="belongs-to-collection">Series Name</meta>  (character data)
    // <meta property="belongs-to-collection" content="Series Name"/>  (attribute, some generators)
    // <meta property="group-position">1</meta>
    if (metaProperty) {
      if (strcmp(metaProperty, "belongs-to-collection") == 0 && self->series.empty()) {
        if (metaContent) {
          self->series = trim(std::string(metaContent, std::min(strlen(metaContent), size_t{MAX_DESCRIPTION_LENGTH})));
        } else {
          self->state = IN_BOOK_SERIES;
          return;
        }
      }
      if (strcmp(metaProperty, "group-position") == 0 && self->seriesIndex.empty()) {
        if (metaContent) {
          self->seriesIndex =
              trim(std::string(metaContent, std::min(strlen(metaContent), size_t{MAX_DESCRIPTION_LENGTH})));
        } else {
          self->state = IN_BOOK_SERIES_INDEX;
          return;
        }
      }
    }

    return;
  }

  if (self->state == IN_MANIFEST && (strcmp(name, "item") == 0 || strcmp(name, "opf:item") == 0)) {
    std::string itemId;
    std::string href;
    std::string mediaType;
    std::string properties;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "id") == 0) {
        itemId = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        href = FsHelpers::normalisePath(self->baseContentPath + atts[i + 1]);
      } else if (strcmp(atts[i], "media-type") == 0) {
        mediaType = atts[i + 1];
      } else if (strcmp(atts[i], "properties") == 0) {
        properties = atts[i + 1];
      }
    }

    // Record index entry for fast lookup later
    if (self->tempItemStore) {
      ItemIndexEntry entry;
      entry.idHash = fnvHash(itemId);
      entry.idLen = static_cast<uint16_t>(itemId.size());
      entry.fileOffset = static_cast<uint32_t>(self->tempItemStore.position());
      self->itemIndex.push_back(entry);
    }

    // Write items down to SD card
    serialization::writeString(self->tempItemStore, itemId);
    serialization::writeString(self->tempItemStore, href);

    if (itemId == self->coverItemId) {
      self->coverItemHref = href;
    }

    if (mediaType == MEDIA_TYPE_NCX) {
      if (self->tocNcxPath.empty()) {
        self->tocNcxPath = href;
      } else {
        LOG_DBG("COF", "Warning: Multiple NCX files found in manifest. Ignoring duplicate: %s", href.c_str());
      }
    }

    // Collect CSS files
    if (mediaType == MEDIA_TYPE_CSS) {
      self->cssFiles.push_back(href);
    }

    // EPUB 3: Check for nav document (properties contains "nav")
    if (!properties.empty() && self->tocNavPath.empty()) {
      // Properties is space-separated, check if "nav" is present as a word
      if (properties == "nav" || properties.find("nav ") == 0 || properties.find(" nav") != std::string::npos) {
        self->tocNavPath = href;
        LOG_DBG("COF", "Found EPUB 3 nav document: %s", href.c_str());
      }
    }

    // EPUB 3: Check for cover image (properties contains "cover-image")
    if (!properties.empty() && self->coverItemHref.empty()) {
      if (properties == "cover-image" || properties.find("cover-image ") == 0 ||
          properties.find(" cover-image") != std::string::npos) {
        self->coverItemHref = href;
      }
    }
    return;
  }

  // NOTE: This relies on spine appearing after item manifest (which is pretty safe as it's part of the EPUB spec)
  // Only run the spine parsing if there's a cache to add it to
  if (self->cache) {
    if (self->state == IN_SPINE && (strcmp(name, "itemref") == 0 || strcmp(name, "opf:itemref") == 0)) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "idref") == 0) {
          const std::string idref = atts[i + 1];
          std::string href;
          bool found = false;

          if (self->useItemIndex) {
            // Fast path: binary search
            uint32_t targetHash = fnvHash(idref);
            uint16_t targetLen = static_cast<uint16_t>(idref.size());

            auto it = std::lower_bound(self->itemIndex.begin(), self->itemIndex.end(),
                                       ItemIndexEntry{targetHash, targetLen, 0},
                                       [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
                                         return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
                                       });

            // Check for match (may need to check a few due to hash collisions)
            while (it != self->itemIndex.end() && it->idHash == targetHash) {
              self->tempItemStore.seek(it->fileOffset);
              std::string itemId;
              serialization::readString(self->tempItemStore, itemId);
              if (itemId == idref) {
                serialization::readString(self->tempItemStore, href);
                found = true;
                break;
              }
              ++it;
            }
          } else {
            // Slow path: linear scan (for small manifests, keeps original behavior)
            // TODO: This lookup is slow as need to scan through all items each time.
            //       It can take up to 200ms per item when getting to 1500 items.
            self->tempItemStore.seek(0);
            std::string itemId;
            while (self->tempItemStore.available()) {
              serialization::readString(self->tempItemStore, itemId);
              serialization::readString(self->tempItemStore, href);
              if (itemId == idref) {
                found = true;
                break;
              }
            }
          }

          if (found && self->cache) {
            self->cache->createSpineEntry(href);
          }
        }
      }
      return;
    }
  }
  // parse the guide
  if (self->state == IN_GUIDE && (strcmp(name, "reference") == 0 || strcmp(name, "opf:reference") == 0)) {
    std::string type;
    std::string guideHref;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "type") == 0) {
        type = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        guideHref = FsHelpers::normalisePath(self->baseContentPath + atts[i + 1]);
      }
    }
    if (!guideHref.empty()) {
      if (type == "text" || (type == "start" && !self->textReferenceHref.empty())) {
        LOG_DBG("COF", "Found %s reference in guide: %s", type.c_str(), guideHref.c_str());
        self->textReferenceHref = guideHref;
      } else if ((type == "cover" || type == "cover-page") && self->guideCoverPageHref.empty()) {
        LOG_DBG("COF", "Found cover reference in guide: %s", guideHref.c_str());
        self->guideCoverPageHref = guideHref;
      }
    }
    return;
  }
}

void XMLCALL ContentOpfParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ContentOpfParser*>(userData);

  if (self->state == IN_BOOK_TITLE) {
    self->title.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_AUTHOR) {
    if (!self->author.empty()) {
      self->author.append(", ");  // Add separator for multiple authors
    }
    self->author.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE) {
    self->language.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_DESCRIPTION) {
    if (self->description.size() < MAX_DESCRIPTION_LENGTH) {
      const size_t remaining = MAX_DESCRIPTION_LENGTH - self->description.size();
      self->description.append(s, std::min(static_cast<size_t>(len), remaining));
    }
    return;
  }

  if (self->state == IN_BOOK_SERIES) {
    if (self->series.size() < MAX_DESCRIPTION_LENGTH) {
      const size_t remaining = MAX_DESCRIPTION_LENGTH - self->series.size();
      self->series.append(s, std::min(static_cast<size_t>(len), remaining));
    }
    return;
  }

  if (self->state == IN_BOOK_SERIES_INDEX) {
    if (self->seriesIndex.size() < MAX_DESCRIPTION_LENGTH) {
      const size_t remaining = MAX_DESCRIPTION_LENGTH - self->seriesIndex.size();
      self->seriesIndex.append(s, std::min(static_cast<size_t>(len), remaining));
    }
    return;
  }
}

void XMLCALL ContentOpfParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)name;

  if (self->state == IN_SPINE && (strcmp(name, "spine") == 0 || strcmp(name, "opf:spine") == 0)) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_GUIDE && (strcmp(name, "guide") == 0 || strcmp(name, "opf:guide") == 0)) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_MANIFEST && (strcmp(name, "manifest") == 0 || strcmp(name, "opf:manifest") == 0)) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_BOOK_TITLE && strcmp(name, "dc:title") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_AUTHOR && strcmp(name, "dc:creator") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE && strcmp(name, "dc:language") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_DESCRIPTION && strcmp(name, "dc:description") == 0) {
    self->description = stripHtml(self->description);
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_SERIES && (strcmp(name, "meta") == 0 || strcmp(name, "opf:meta") == 0)) {
    self->series = trim(self->series);
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_SERIES_INDEX && (strcmp(name, "meta") == 0 || strcmp(name, "opf:meta") == 0)) {
    self->seriesIndex = trim(self->seriesIndex);
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = START;
    return;
  }
}
