#include "ContentOpfParser.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <XmlParserUtils.h>

#include <cctype>
#include <cstring>
#include <string_view>

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

bool isXmlWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Metadata text comes straight from the (untrusted) OPF; unbounded growth on
// a multi-megabyte title would exhaust the heap. Downstream consumers truncate
// far below this anyway, so overflow is clamped, not fatal.
constexpr size_t MAX_METADATA_TEXT = 512;

void appendMetadataText(std::string& out, const XML_Char* text, const int len, bool& spacePending,
                        bool* separatorPending = nullptr) {
  if (out.size() >= MAX_METADATA_TEXT) return;  // already clamped and logged
  for (int i = 0; i < len; i++) {
    const char c = text[i];
    if (isXmlWhitespace(c)) {
      spacePending = true;
      continue;
    }

    if (out.size() >= MAX_METADATA_TEXT) {
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

// Attribute values go through the same clamp and whitespace rules as text.
void assignMetadataAttribute(std::string& out, const char* value) {
  out.clear();
  bool spacePending = false;
  appendMetadataText(out, value, static_cast<int>(strlen(value)), spacePending);
}

bool asciiEqualsIgnoreCase(const std::string_view a, const std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// Calibre writes "3.0"; show "3". Anything that is not a plain decimal is kept as written.
void normaliseSeriesIndex(std::string& index) {
  const size_t dot = index.find('.');
  if (dot == std::string::npos) return;
  for (size_t i = 0; i < index.size(); i++) {
    if (i != dot && !std::isdigit(static_cast<unsigned char>(index[i]))) return;
  }
  while (index.size() > dot + 1 && index.back() == '0') index.pop_back();
  if (index.size() == dot + 1 && dot > 0) index.pop_back();
}

// Bounds on the transient ExtendedState. Books past them lose only the
// overflow: later creators, collections, forward refinements or tags.
constexpr uint8_t MAX_TRACKED_CREATORS = 4;
constexpr uint8_t MAX_TRACKED_COLLECTIONS = 4;
constexpr uint8_t MAX_PENDING_REFINEMENTS = 8;
constexpr uint8_t MAX_TAGS = 16;

enum class MetaProperty : uint8_t {
  None,
  FileAs,
  Role,
  GroupPosition,
  CollectionType,
  BelongsToCollection,
  Genre,
};

MetaProperty metaPropertyFrom(const char* property) {
  if (strcmp(property, "file-as") == 0) return MetaProperty::FileAs;
  if (strcmp(property, "role") == 0) return MetaProperty::Role;
  if (strcmp(property, "group-position") == 0) return MetaProperty::GroupPosition;
  if (strcmp(property, "collection-type") == 0) return MetaProperty::CollectionType;
  if (strcmp(property, "belongs-to-collection") == 0) return MetaProperty::BelongsToCollection;
  if (strcmp(property, "schema:genre") == 0) return MetaProperty::Genre;
  return MetaProperty::None;
}
}  // namespace

struct ContentOpfParser::ExtendedState {
  struct Creator {
    std::string id;
    std::string fileAs;
    std::string role;
  };
  struct Collection {
    std::string id;
    std::string name;
    std::string type;
    std::string position;
  };
  // An EPUB 3 refinement whose target id had not been seen yet.
  struct Refinement {
    std::string targetId;
    std::string value;
    MetaProperty property = MetaProperty::None;
  };

  Creator creators[MAX_TRACKED_CREATORS];
  Collection collections[MAX_TRACKED_COLLECTIONS];
  Refinement pending[MAX_PENDING_REFINEMENTS];
  uint8_t creatorCount = 0;
  uint8_t collectionCount = 0;
  uint8_t pendingCount = 0;
  uint8_t tagCount = 0;

  std::string titleId;
  std::string titleFileAs;
  std::string calibreTitleSort;
  std::string calibreSeries;
  std::string calibreSeriesIndex;

  // The <dc:subject> or <meta> currently collecting text.
  std::string text;
  std::string metaId;
  std::string metaRefines;
  MetaProperty metaProperty = MetaProperty::None;
  bool textSpacePending = false;

  // True when targetId names the title, a tracked creator or a collection.
  bool applyRefinement(const std::string& targetId, const MetaProperty property, std::string& value) {
    if (targetId.empty()) return false;
    if (targetId == titleId) {
      if (property == MetaProperty::FileAs && titleFileAs.empty()) titleFileAs = std::move(value);
      return true;
    }
    for (uint8_t i = 0; i < creatorCount; i++) {
      Creator& creator = creators[i];
      if (creator.id != targetId) continue;
      if (property == MetaProperty::FileAs && creator.fileAs.empty()) creator.fileAs = std::move(value);
      if (property == MetaProperty::Role) creator.role = std::move(value);
      return true;
    }
    for (uint8_t i = 0; i < collectionCount; i++) {
      Collection& collection = collections[i];
      if (collection.id != targetId) continue;
      if (property == MetaProperty::CollectionType) collection.type = std::move(value);
      if (property == MetaProperty::GroupPosition) collection.position = std::move(value);
      return true;
    }
    return false;
  }
};

ContentOpfParser::ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath,
                                   const size_t xmlSize, BookMetadataCache* cache, const bool metadataOnly)
    : cachePath(cachePath),
      baseContentPath(baseContentPath),
      remainingSize(xmlSize),
      cache(cache),
      metadataOnly(metadataOnly) {}

bool ContentOpfParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_DBG("COF", "Couldn't allocate memory for parser");
    return false;
  }

  ext = makeUniqueNoThrow<ExtendedState>();
  if (!ext) {
    LOG_ERR("COF", "OOM: extended metadata state; sort keys, series and tags skipped");
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

void XMLCALL ContentOpfParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)atts;

  if (self->metadataOnly && self->metadataComplete) {
    return;
  }
  if (self->metadataOnly && (xmlLocalNameEquals(name, "manifest") || xmlLocalNameEquals(name, "spine") ||
                             xmlLocalNameEquals(name, "guide"))) {
    self->finalizeExtendedMetadata();
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
      if (self->ext) {
        for (int i = 0; atts[i]; i += 2) {
          if (strcmp(atts[i], "id") == 0) {
            self->ext->titleId = atts[i + 1];
          } else if (xmlLocalNameEquals(atts[i], "file-as")) {
            assignMetadataAttribute(self->ext->titleFileAs, atts[i + 1]);
          }
        }
      }
    }
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "creator")) {
    self->state = IN_BOOK_AUTHOR;
    self->metadataSpacePending = false;
    self->authorSeparatorPending = !self->author.empty();
    if (self->ext && self->ext->creatorCount < MAX_TRACKED_CREATORS) {
      // EPUB 2 carries file-as and role as opf: attributes; EPUB 3 refines them by id.
      auto& creator = self->ext->creators[self->ext->creatorCount++];
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "id") == 0) {
          creator.id = atts[i + 1];
        } else if (xmlLocalNameEquals(atts[i], "file-as")) {
          assignMetadataAttribute(creator.fileAs, atts[i + 1]);
        } else if (xmlLocalNameEquals(atts[i], "role")) {
          assignMetadataAttribute(creator.role, atts[i + 1]);
        }
      }
    }
    return;
  }

  if (self->state == IN_METADATA && self->ext && xmlLocalNameEquals(name, "subject")) {
    self->state = IN_BOOK_SUBJECT;
    self->ext->text.clear();
    self->ext->textSpacePending = false;
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "language")) {
    self->state = IN_BOOK_LANGUAGE;
    self->metadataSpacePending = false;
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
    const char* content = nullptr;
    const char* property = nullptr;
    const char* refines = nullptr;
    const char* id = nullptr;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "name") == 0) {
        metaName = atts[i + 1];
      } else if (strcmp(atts[i], "content") == 0) {
        content = atts[i + 1];
      } else if (strcmp(atts[i], "property") == 0) {
        property = atts[i + 1];
      } else if (strcmp(atts[i], "refines") == 0) {
        refines = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        id = atts[i + 1];
      }
    }

    if (metaName && strcmp(metaName, "cover") == 0) {
      self->coverItemId = content ? content : "";
    }
    if (!self->ext) {
      return;
    }

    if (metaName && content) {
      if (strcmp(metaName, "calibre:series") == 0) {
        assignMetadataAttribute(self->ext->calibreSeries, content);
      } else if (strcmp(metaName, "calibre:series_index") == 0) {
        assignMetadataAttribute(self->ext->calibreSeriesIndex, content);
      } else if (strcmp(metaName, "calibre:title_sort") == 0) {
        assignMetadataAttribute(self->ext->calibreTitleSort, content);
      }
    }

    if (property) {
      const MetaProperty metaProperty = metaPropertyFrom(property);
      if (metaProperty != MetaProperty::None) {
        self->state = IN_META_TEXT;
        self->ext->metaProperty = metaProperty;
        self->ext->metaId = id ? id : "";
        self->ext->metaRefines = refines ? (refines[0] == '#' ? refines + 1 : refines) : "";
        self->ext->text.clear();
        self->ext->textSpacePending = false;
      }
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

    if (itemId == self->coverItemId) {
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

  // Both states are only entered while ext is allocated.
  if (self->state == IN_BOOK_SUBJECT || self->state == IN_META_TEXT) {
    appendMetadataText(self->ext->text, s, len, self->ext->textSpacePending);
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
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_AUTHOR && xmlLocalNameEquals(name, "creator")) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE && xmlLocalNameEquals(name, "language")) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_SUBJECT && xmlLocalNameEquals(name, "subject")) {
    self->state = IN_METADATA;
    self->addTag(self->ext->text);
    return;
  }

  if (self->state == IN_META_TEXT && xmlLocalNameEquals(name, "meta")) {
    self->state = IN_METADATA;
    self->endMetaText();
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "metadata")) {
    self->state = IN_PACKAGE;
    self->finalizeExtendedMetadata();
    self->metadataComplete = true;
    return;
  }

  if (self->state == IN_PACKAGE && xmlLocalNameEquals(name, "package")) {
    self->state = START;
    return;
  }
}

void ContentOpfParser::addTag(std::string& value) {
  if (value.empty() || ext->tagCount >= MAX_TAGS) return;

  // dc:subject and schema:genre often repeat each other ("Fiction" twice).
  const std::string_view existing(tags);
  size_t start = 0;
  while (start < existing.size()) {
    size_t end = existing.find(TAG_SEPARATOR, start);
    if (end == std::string_view::npos) end = existing.size();
    if (asciiEqualsIgnoreCase(existing.substr(start, end - start), value)) return;
    start = end + 1;
  }

  const size_t needed = value.size() + (tags.empty() ? 0 : 1);
  if (tags.size() + needed > MAX_METADATA_TEXT) {
    LOG_DBG("COF", "Tag list full; dropping: %s", value.c_str());
    return;
  }
  if (!tags.empty()) tags.push_back(TAG_SEPARATOR);
  tags += value;
  ext->tagCount++;
}

void ContentOpfParser::endMetaText() {
  ExtendedState& e = *ext;
  if (e.text.empty()) return;

  if (e.metaRefines.empty()) {
    if (e.metaProperty == MetaProperty::BelongsToCollection && e.collectionCount < MAX_TRACKED_COLLECTIONS) {
      auto& collection = e.collections[e.collectionCount++];
      collection.id = e.metaId;
      collection.name = std::move(e.text);
    } else if (e.metaProperty == MetaProperty::Genre) {
      addTag(e.text);
    }
    // file-as, role and the like mean nothing without a target.
    return;
  }

  // A refined belongs-to-collection is a sub-collection of another one; only
  // top-level collections name a series.
  if (e.metaProperty == MetaProperty::BelongsToCollection || e.metaProperty == MetaProperty::Genre) return;

  if (!e.applyRefinement(e.metaRefines, e.metaProperty, e.text) && e.pendingCount < MAX_PENDING_REFINEMENTS) {
    auto& refinement = e.pending[e.pendingCount++];
    refinement.targetId = e.metaRefines;
    refinement.value = std::move(e.text);
    refinement.property = e.metaProperty;
  }
}

void ContentOpfParser::finalizeExtendedMetadata() {
  if (!ext) return;
  ExtendedState& e = *ext;

  for (uint8_t i = 0; i < e.pendingCount; i++) {
    e.applyRefinement(e.pending[i].targetId, e.pending[i].property, e.pending[i].value);
  }

  titleSort = !e.titleFileAs.empty() ? std::move(e.titleFileAs) : std::move(e.calibreTitleSort);

  const ExtendedState::Creator* primary = nullptr;
  for (uint8_t i = 0; i < e.creatorCount; i++) {
    if (e.creators[i].role.empty() || asciiEqualsIgnoreCase(e.creators[i].role, "aut")) {
      primary = &e.creators[i];
      break;
    }
  }
  if (!primary && e.creatorCount > 0) primary = &e.creators[0];
  if (primary) authorSort = primary->fileAs;

  if (!e.calibreSeries.empty()) {
    series = std::move(e.calibreSeries);
    seriesIndex = std::move(e.calibreSeriesIndex);
  } else {
    // An explicit "series" beats an untyped collection; a "set" is a publisher
    // grouping, not a reading order, and never names the series.
    const ExtendedState::Collection* best = nullptr;
    int bestRank = 0;
    for (uint8_t i = 0; i < e.collectionCount; i++) {
      const auto& collection = e.collections[i];
      const int rank = collection.type == "series" ? 2 : (collection.type.empty() ? 1 : 0);
      if (rank > bestRank) {
        best = &collection;
        bestRank = rank;
      }
    }
    if (best) {
      series = best->name;
      seriesIndex = best->position;
    }
  }
  normaliseSeriesIndex(seriesIndex);

  ext.reset();
}
