#include "ContentOpfParser.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>
#include <XmlParserUtils.h>

#include <cctype>
#include <cstring>

#include "Epub/BookMetadataCache.h"

namespace {
constexpr char MEDIA_TYPE_NCX[] = "application/x-dtbncx+xml";
constexpr char MEDIA_TYPE_CSS[] = "text/css";
constexpr char MEDIA_TYPE_IMAGE_PREFIX[] = "image/";
constexpr char itemCacheFile[] = "/.items.bin";

bool startsWithImageMediaType(const std::string& mediaType) {
  constexpr size_t prefixLen = sizeof(MEDIA_TYPE_IMAGE_PREFIX) - 1;
  if (mediaType.size() < prefixLen) {
    return false;
  }

  for (size_t i = 0; i < prefixLen; ++i) {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(mediaType[i])));
    if (c != MEDIA_TYPE_IMAGE_PREFIX[i]) {
      return false;
    }
  }

  return true;
}

// Metadata text comes straight from the (untrusted) OPF; unbounded growth on
// a multi-megabyte title would exhaust the heap. Downstream consumers truncate
// far below this anyway, so overflow is clamped, not fatal.
constexpr size_t MAX_METADATA_TEXT = 512;

void assignBoundedAttribute(std::string& out, const char* value) {
  out.clear();
  if (value == nullptr) {
    return;
  }

  size_t length = 0;
  while (length < MAX_METADATA_TEXT && value[length] != '\0') {
    length++;
  }
  const size_t safeLength = static_cast<size_t>(utf8SafeTruncateBuffer(value, static_cast<int>(length)));
  out.assign(value, safeLength);
  if (value[length] != '\0') {
    LOG_DBG("COF", "Metadata attribute exceeds %u bytes; truncating", static_cast<unsigned>(MAX_METADATA_TEXT));
  }
}

void truncateToCodepoint(std::string& text) {
  text.resize(static_cast<size_t>(utf8SafeTruncateBuffer(text.data(), static_cast<int>(text.size()))));
}

void collapseAttributeText(std::string& text) {
  size_t writeIndex = 0;
  bool spacePending = false;
  for (const char c : text) {
    if (isAsciiWhitespace(c)) {
      spacePending = true;
      continue;
    }
    if (spacePending && writeIndex != 0) {
      text[writeIndex++] = ' ';
    }
    spacePending = false;
    text[writeIndex++] = c;
  }
  text.resize(writeIndex);
}

void appendMetadataText(std::string& out, const XML_Char* text, const int len, bool& spacePending,
                        bool* separatorPending = nullptr) {
  if (out.size() >= MAX_METADATA_TEXT) return;  // already clamped and logged
  for (int i = 0; i < len; i++) {
    const char c = text[i];
    if (isAsciiWhitespace(c)) {
      spacePending = true;
      continue;
    }

    size_t prefixLength = 0;
    if (separatorPending != nullptr && *separatorPending) {
      prefixLength = 2;
    } else if (spacePending && !out.empty()) {
      prefixLength = 1;
    }
    if (out.size() + prefixLength + 1 > MAX_METADATA_TEXT) {
      LOG_DBG("COF", "Metadata text exceeds %u bytes; truncating", static_cast<unsigned>(MAX_METADATA_TEXT));
      return;
    }
    if (separatorPending != nullptr && *separatorPending) {
      out.append(", ");
      *separatorPending = false;
      spacePending = false;
    } else if (spacePending && !out.empty()) {
      out.push_back(' ');
    }
    spacePending = false;
    out.push_back(c);
  }
}

std::string lowerAscii(std::string value) {
  for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

bool hasMetadataPrefix(const std::string& value, const char* prefix) {
  const std::string lower = lowerAscii(value);
  const std::string wanted = lowerAscii(prefix);
  return lower.rfind(wanted, 0) == 0 || lower.rfind("urn:" + wanted + ":", 0) == 0;
}

std::string stripMetadataPrefix(const std::string& value, const char* prefix) {
  const std::string lower = lowerAscii(value);
  const std::string wanted = lowerAscii(prefix);
  size_t offset = 0;
  const std::string urnPrefix = "urn:" + wanted + ":";
  if (lower.rfind(urnPrefix, 0) == 0) {
    offset = urnPrefix.size();
  } else if (lower.rfind(wanted, 0) == 0) {
    offset = wanted.size();
  } else {
    return value;
  }
  while (offset < value.size() && (value[offset] == ':' || value[offset] == ' ' || value[offset] == '	')) ++offset;
  return value.substr(offset);
}
}  // namespace

// Combined FNV-1a and SDBM hash (for collision reduction)
ContentOpfParser::AttributeFingerprint ContentOpfParser::fingerprint(const char* value) {
  AttributeFingerprint result;
  if (value == nullptr || value[0] == '\0') {
    return result;
  }

  result.fnv = 2166136261u;
  result.sdbm = 0;
  result.present = true;
  for (const char* cursor = value; *cursor != '\0'; cursor++) {
    const auto byte = static_cast<unsigned char>(*cursor);
    result.fnv ^= byte;
    result.fnv *= 16777619u;
    result.sdbm = byte + (result.sdbm << 6) + (result.sdbm << 16) - result.sdbm;
    result.length++;
  }
  return result;
}

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
  destroyXmlParser(parser);
  if (metadataOnly || !cache) {
    return;
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
      destroyXmlParser(parser);
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead) == XML_STATUS_ERROR) {
      LOG_DBG("COF", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;

    if (metadataOnly && metadataComplete) {
      const size_t processed = size - remainingInBuffer;
      return processed < size ? processed : size - 1;
    }
  }

  return size;
}

void ContentOpfParser::enterSingleValueElement(const ParserState capturing, const std::string& value) {
  if (!value.empty()) {
    return;
  }
  state = capturing;
  metadataSpacePending = false;
}

void XMLCALL ContentOpfParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  if (self->metadataOnly && self->metadataComplete) {
    return;
  }
  if (self->metadataOnly && (xmlLocalNameEquals(name, "manifest") || xmlLocalNameEquals(name, "spine") ||
                             xmlLocalNameEquals(name, "guide"))) {
    self->metadataComplete = true;
    return;
  }

  if (self->state == START && xmlLocalNameEquals(name, "package")) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && xmlLocalNameEquals(name, "metadata")) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "title")) {
    // Only capture the first title element; subsequent ones are subtitles
    if (self->title.empty()) {
      self->state = IN_BOOK_TITLE;
      self->metadataSpacePending = false;
    }
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "creator")) {
    self->state = IN_BOOK_AUTHOR;
    self->metadataSpacePending = false;
    self->authorSeparatorPending = !self->author.empty();
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "language")) {
    self->enterSingleValueElement(IN_BOOK_LANGUAGE, self->language);
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "publisher")) {
    self->enterSingleValueElement(IN_BOOK_PUBLISHER, self->publisher);
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "subject")) {
    self->enterSingleValueElement(IN_BOOK_SUBJECT, self->subject);
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "identifier")) {
    self->state = IN_BOOK_IDENTIFIER;
    self->identifierText.clear();
    self->identifierScheme.clear();
    self->metadataSpacePending = false;
    for (int i = 0; atts[i]; i += 2) {
      if (xmlLocalNameEquals(atts[i], "scheme")) assignBoundedAttribute(self->identifierScheme, atts[i + 1]);
    }
    return;
  }

  if (self->state == IN_PACKAGE && xmlLocalNameEquals(name, "manifest")) {
    self->state = IN_MANIFEST;
    if (self->cache && !Storage.openFileForWrite("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for writing. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_PACKAGE && xmlLocalNameEquals(name, "spine")) {
    self->state = IN_SPINE;
    if (self->cache && !Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }

    // Sort the (unconditionally-built) item index so every idref lookup uses binary
    // search. Without this, small/medium manifests fell back to an O(spine × manifest)
    // linear rescan of .items.bin per itemref (up to ~200ms/item at large scale).
    if (!self->itemIndex.empty()) {
      std::sort(self->itemIndex.begin(), self->itemIndex.end(), [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
        return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
      });
      self->useItemIndex = true;
      LOG_DBG("COF", "Using fast index for %zu manifest items", self->itemIndex.size());
    }
    return;
  }

  if (self->state == IN_PACKAGE && xmlLocalNameEquals(name, "guide")) {
    self->state = IN_GUIDE;
    // TODO Remove print
    LOG_DBG("COF", "Entering guide state.");
    if (self->cache && !Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "meta")) {
    const char* metaName = nullptr;
    const char* property = nullptr;
    const char* content = nullptr;
    const char* id = nullptr;
    const char* refines = nullptr;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "name") == 0) {
        metaName = atts[i + 1];
      } else if (strcmp(atts[i], "content") == 0) {
        content = atts[i + 1];
      } else if (strcmp(atts[i], "property") == 0) {
        property = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        id = atts[i + 1];
      } else if (strcmp(atts[i], "refines") == 0) {
        refines = atts[i + 1];
      }
    }

    if (metaName != nullptr && strcmp(metaName, "cover") == 0) {
      self->coverItemId = fingerprint(content);
    }

    // EPUB 2 Calibre series values are attributes
    if (metaName != nullptr && strcmp(metaName, "calibre:series") == 0) {
      if (self->calibreSeries.empty()) {
        assignBoundedAttribute(self->calibreSeries, content);
      }
      return;
    }
    if (metaName != nullptr && strcmp(metaName, "calibre:series_index") == 0) {
      if (self->calibreSeriesIndex.empty()) {
        assignBoundedAttribute(self->calibreSeriesIndex, content);
      }
      return;
    }

    const bool isCollection = property != nullptr && strcmp(property, "belongs-to-collection") == 0;
    const bool hasLocalRefines = refines != nullptr && refines[0] == '#' && refines[1] != '\0';
    const bool isCollectionType = hasLocalRefines && property != nullptr && strcmp(property, "collection-type") == 0;
    const bool isGroupPosition = hasLocalRefines && property != nullptr && strcmp(property, "group-position") == 0;
    if (isCollection || isCollectionType || isGroupPosition) {
      self->metaText.clear();
      self->metaId = fingerprint(id);
      self->metaRefines = hasLocalRefines ? fingerprint(refines + 1) : AttributeFingerprint{};
      self->metaIsCollection = isCollection;
      self->metaIsCollectionType = isCollectionType;
      self->metadataSpacePending = false;
      self->state = IN_META_VALUE;
    }
    return;
  }

  if (self->state == IN_MANIFEST && xmlLocalNameEquals(name, "item")) {
    std::string itemId;
    std::string href;
    std::string mediaType;
    std::string properties;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "id") == 0) {
        itemId = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
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

    if (self->tempItemStore) {
      serialization::writeString(self->tempItemStore, itemId);
      serialization::writeString(self->tempItemStore, href);
    }

    if (self->coverItemId.present && fingerprint(itemId.c_str()) == self->coverItemId) {
      // Some EPUBs set meta name="cover" to an XHTML wrapper item.
      // Only treat it as a cover image when the manifest media-type is image/*.
      if (startsWithImageMediaType(mediaType)) {
        self->coverItemHref = href;
      } else {
        LOG_DBG("COF", "Ignoring meta cover item '%s' with non-image media type: %s", itemId.c_str(),
                mediaType.c_str());
      }
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
    if (self->state == IN_SPINE && xmlLocalNameEquals(name, "itemref")) {
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
            // Fallback linear scan, only reached when the index is empty (no manifest
            // items). The fast binary-search path above is used for all real manifests.
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
  if (self->state == IN_GUIDE && xmlLocalNameEquals(name, "reference")) {
    std::string type;
    std::string guideHref;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "type") == 0) {
        type = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        guideHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      }
    }
    if (!guideHref.empty()) {
      // EPUB 2 guides often mark every content file as "text", so that type
      // does not identify a reliable first-reading location. Only use the
      // explicit "start" semantic; otherwise the reader opens at spine index 0.
      if (type == "start" && !self->hasExplicitStartReference) {
        LOG_DBG("COF", "Found %s reference in guide: %s", type.c_str(), guideHref.c_str());
        self->textReferenceHref = guideHref;
        self->hasExplicitStartReference = type == "start";
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

  if (self->metadataOnly && self->metadataComplete) {
    return;
  }

  if (self->state == IN_BOOK_TITLE) {
    appendMetadataText(self->title, s, len, self->metadataSpacePending);
    return;
  }

  if (self->state == IN_BOOK_AUTHOR) {
    appendMetadataText(self->author, s, len, self->metadataSpacePending, &self->authorSeparatorPending);
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE) {
    appendMetadataText(self->language, s, len, self->metadataSpacePending);
    return;
  }

  if (self->state == IN_BOOK_IDENTIFIER) {
    appendMetadataText(self->identifierText, s, len, self->metadataSpacePending);
    return;
  }

  if (self->state == IN_BOOK_PUBLISHER) {
    appendMetadataText(self->publisher, s, len, self->metadataSpacePending);
    return;
  }

  if (self->state == IN_BOOK_SUBJECT) {
    appendMetadataText(self->subject, s, len, self->metadataSpacePending);
    return;
  }

  if (self->state == IN_META_VALUE) {
    appendMetadataText(self->metaText, s, len, self->metadataSpacePending);
    return;
  }
}

void XMLCALL ContentOpfParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)name;

  if (self->metadataOnly && self->metadataComplete) {
    return;
  }

  if (self->state == IN_SPINE && xmlLocalNameEquals(name, "spine")) {
    self->state = IN_PACKAGE;
    if (self->tempItemStore) self->tempItemStore.close();
    return;
  }

  if (self->state == IN_GUIDE && xmlLocalNameEquals(name, "guide")) {
    self->state = IN_PACKAGE;
    if (self->tempItemStore) self->tempItemStore.close();
    return;
  }

  if (self->state == IN_MANIFEST && xmlLocalNameEquals(name, "manifest")) {
    self->state = IN_PACKAGE;
    if (self->tempItemStore) self->tempItemStore.close();
    return;
  }

  if (self->state == IN_BOOK_TITLE && xmlLocalNameEquals(name, "title")) {
    truncateToCodepoint(self->title);
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_AUTHOR && xmlLocalNameEquals(name, "creator")) {
    truncateToCodepoint(self->author);
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE && xmlLocalNameEquals(name, "language")) {
    truncateToCodepoint(self->language);
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_IDENTIFIER && xmlLocalNameEquals(name, "identifier")) {
    const std::string scheme = lowerAscii(self->identifierScheme);
    if (self->isbn.empty() &&
        (scheme.find("isbn") != std::string::npos || hasMetadataPrefix(self->identifierText, "isbn"))) {
      self->isbn = stripMetadataPrefix(self->identifierText, "isbn");
    }
    if (self->asin.empty() && (scheme.find("asin") != std::string::npos || scheme == "amazon" ||
                               hasMetadataPrefix(self->identifierText, "asin"))) {
      self->asin = stripMetadataPrefix(self->identifierText, "asin");
    }
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_PUBLISHER && xmlLocalNameEquals(name, "publisher")) {
    truncateToCodepoint(self->publisher);
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_SUBJECT && xmlLocalNameEquals(name, "subject")) {
    truncateToCodepoint(self->subject);
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_META_VALUE && xmlLocalNameEquals(name, "meta")) {
    truncateToCodepoint(self->metaText);
    self->state = IN_METADATA;
    if (self->metaIsCollection) {
      if (self->collectionCount < MAX_COLLECTIONS) {
        self->collections[self->collectionCount].id = self->metaId;
        self->collections[self->collectionCount].name = self->metaText;
        self->collectionCount++;
      }
    } else if (self->refineCount < MAX_REFINES) {
      StagedRefine& refine = self->refines[self->refineCount];
      refine.target = self->metaRefines;
      if (self->metaIsCollectionType) {
        refine.type = asciiEqualsIgnoreCase(self->metaText, "series") ? CollectionType::Series : CollectionType::Other;
      } else {
        refine.position = self->metaText;
      }
      self->refineCount++;
    }
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "metadata")) {
    self->state = IN_PACKAGE;
    self->resolveSeries();
    self->metadataComplete = true;
    return;
  }

  if (self->state == IN_PACKAGE && xmlLocalNameEquals(name, "package")) {
    self->state = START;
    return;
  }
}

void ContentOpfParser::resolveCollection(const AttributeFingerprint& id, CollectionType& type,
                                         std::string& position) const {
  type = CollectionType::Untyped;
  position.clear();
  if (!id.present) {
    return;
  }

  for (size_t i = 0; i < refineCount; i++) {
    if (refines[i].target != id) {
      continue;
    }
    if (refines[i].type != CollectionType::Untyped) {
      type = refines[i].type;
    }
    if (!refines[i].position.empty()) {
      position = refines[i].position;
    }
  }
}

void ContentOpfParser::resolveSeries() {
  // prefer Calibre series metadata over EPUB 3 collections
  if (!calibreSeries.empty()) {
    collapseAttributeText(calibreSeries);
    if (!calibreSeries.empty()) {
      collapseAttributeText(calibreSeriesIndex);
      series = std::move(calibreSeries);
      seriesIndexText = std::move(calibreSeriesIndex);
      return;
    }
  }

  // prefer the first series-typed collection followed by the first untyped collection
  // ignore collections explicitly marked as another type
  std::string chosenName;
  std::string chosenPosition;
  for (size_t c = 0; c < collectionCount; c++) {
    CollectionType type = CollectionType::Untyped;
    std::string position;
    resolveCollection(collections[c].id, type, position);
    if (type == CollectionType::Other) {
      continue;
    }
    if (collections[c].name.empty()) {
      continue;
    }
    if (!chosenName.empty() && type != CollectionType::Series) {
      continue;
    }
    chosenName = collections[c].name;
    chosenPosition = position;
    if (type == CollectionType::Series) {
      break;
    }
  }
  if (chosenName.empty()) {
    return;
  }

  series = std::move(chosenName);
  seriesIndexText = std::move(chosenPosition);
}
