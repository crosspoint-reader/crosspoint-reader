#include "ContentOpfParser.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Serialization.h>

#include "../BookMetadataCache.h"

namespace {
constexpr char MEDIA_TYPE_NCX[] = "application/x-dtbncx+xml";
constexpr char MEDIA_TYPE_CSS[] = "text/css";
constexpr char itemCacheFile[] = "/.items.bin";

std::string trim(const std::string& value) {
  size_t start = 0;
  while (start < value.size() && isspace(static_cast<unsigned char>(value[start]))) {
    start++;
  }

  size_t end = value.size();
  while (end > start && isspace(static_cast<unsigned char>(value[end - 1]))) {
    end--;
  }

  return value.substr(start, end - start);
}

std::string normalizeRefines(const std::string& refines) {
  if (!refines.empty() && refines[0] == '#') {
    return refines.substr(1);
  }
  return refines;
}

bool isMetadataMetaTag(const XML_Char* name) { return strcmp(name, "meta") == 0 || strcmp(name, "opf:meta") == 0; }
}  // namespace

void ContentOpfParser::processEpub3Meta(const std::string& property, const std::string& refines, const std::string& id,
                                        const std::string& value) {
  std::string cleanValue = trim(value);
  if (cleanValue.empty()) {
    return;
  }

  if (property == "belongs-to-collection") {
    epub3Collections.push_back({id, std::move(cleanValue)});
    return;
  }

  std::string normalizedRefines = normalizeRefines(refines);
  if (property == "group-position") {
    epub3GroupPositions.push_back({std::move(normalizedRefines), std::move(cleanValue)});
    return;
  }

  if (property == "collection-type") {
    epub3CollectionTypes.push_back({std::move(normalizedRefines), std::move(cleanValue)});
  }
}

// Apply EPUB3 belongs-to-collection metadata collected during parsing.
// Selects a collection to use as the series, preferring one explicitly typed as
// "series", and resolves the series index from the matching group-position.
//
// Example OPF with an explicit collection-type:
//   <meta property="belongs-to-collection" id="col1">The Stormlight Archive</meta>
//   <meta property="collection-type" refines="#col1">series</meta>
//   <meta property="group-position"  refines="#col1">2</meta>
//
// Example OPF without collection-type (treated as series by default):
//   <meta property="belongs-to-collection" id="col1">Harry Potter</meta>
//   <meta property="group-position"         refines="#col1">3</meta>
//
// Example OPF with a bare group-position (no refines, no id — fallback):
//   <meta property="belongs-to-collection">Discworld</meta>
//   <meta property="group-position">12</meta>
void ContentOpfParser::applyEpub3Metadata() {
  const Epub3CollectionEntry* selectedCollection = nullptr;

  for (const auto& collection : epub3Collections) {
    const std::string collectionId = normalizeRefines(collection.id);
    bool isSeriesCollection = false;
    for (const auto& typed : epub3CollectionTypes) {
      if (typed.first == collectionId && typed.second == "series") {
        isSeriesCollection = true;
        break;
      }
    }
    if (isSeriesCollection) {
      selectedCollection = &collection;
      break;
    }
  }

  if (!selectedCollection && !epub3Collections.empty()) {
    selectedCollection = &epub3Collections.front();
  }

  if (series.empty() && selectedCollection) {
    series = selectedCollection->value;
  }

  if (seriesIndex.empty()) {
    if (selectedCollection) {
      const std::string selectedId = normalizeRefines(selectedCollection->id);
      for (const auto& groupPosition : epub3GroupPositions) {
        if (!selectedId.empty() && groupPosition.first == selectedId) {
          seriesIndex = groupPosition.second;
          break;
        }
      }
    }

    if (seriesIndex.empty()) {
      for (const auto& groupPosition : epub3GroupPositions) {
        if (groupPosition.first.empty()) {
          seriesIndex = groupPosition.second;
          break;
        }
      }
    }
  }
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
  if (Storage.exists((cachePath + itemCacheFile).c_str())) {
    Storage.remove((cachePath + itemCacheFile).c_str());
  }
  itemIndex.clear();
  itemIndex.shrink_to_fit();
  useItemIndex = false;
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

  if (self->state == IN_METADATA && isMetadataMetaTag(name)) {
    bool isCover = false;
    bool isSeries = false;
    bool isSeriesIndex = false;
    std::string metaContent;
    std::string metaProperty;
    std::string metaRefines;
    std::string metaId;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "name") == 0) {
        if (strcmp(atts[i + 1], "cover") == 0) {
          isCover = true;
        } else if (strcmp(atts[i + 1], "calibre:series") == 0) {
          isSeries = true;
        } else if (strcmp(atts[i + 1], "calibre:series_index") == 0) {
          isSeriesIndex = true;
        }
      } else if (strcmp(atts[i], "content") == 0) {
        metaContent = atts[i + 1];
      } else if (strcmp(atts[i], "property") == 0) {
        metaProperty = atts[i + 1];
      } else if (strcmp(atts[i], "refines") == 0) {
        metaRefines = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        metaId = atts[i + 1];
      }
    }

    if (isCover) {
      self->coverItemId = metaContent;
    } else if (isSeries && !metaContent.empty()) {
      self->series = trim(metaContent);
    } else if (isSeriesIndex && !metaContent.empty()) {
      self->seriesIndex = trim(metaContent);
    }

    if (!metaProperty.empty()) {
      if (!metaContent.empty()) {
        self->processEpub3Meta(metaProperty, metaRefines, metaId, metaContent);
      } else {
        self->activeMetaProperty = metaProperty;
        self->activeMetaRefines = metaRefines;
        self->activeMetaId = metaId;
        self->activeMetaText.clear();
        self->state = IN_META_PROPERTY;
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

  if (self->state == IN_META_PROPERTY) {
    self->activeMetaText.append(s, len);
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

  if (self->state == IN_META_PROPERTY && isMetadataMetaTag(name)) {
    self->processEpub3Meta(self->activeMetaProperty, self->activeMetaRefines, self->activeMetaId, self->activeMetaText);
    self->activeMetaProperty.clear();
    self->activeMetaRefines.clear();
    self->activeMetaId.clear();
    self->activeMetaText.clear();
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->applyEpub3Metadata();
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = START;
    return;
  }
}
