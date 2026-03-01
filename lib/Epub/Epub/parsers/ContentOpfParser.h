#pragma once
#include <Print.h>

#include <algorithm>
#include <vector>

#include "Epub.h"
#include "expat.h"

class BookMetadataCache;

class ContentOpfParser final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_META_PROPERTY,
    IN_BOOK_TITLE,
    IN_BOOK_AUTHOR,
    IN_BOOK_LANGUAGE,
    IN_MANIFEST,
    IN_SPINE,
    IN_GUIDE,
  };

  const std::string& cachePath;
  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;
  FsFile tempItemStore;
  std::string coverItemId;

  // Index for fast idref→href lookup (used only for large EPUBs)
  struct ItemIndexEntry {
    uint32_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t fileOffset;  // offset in .items.bin
  };
  std::vector<ItemIndexEntry> itemIndex;
  bool useItemIndex = false;

  struct Epub3CollectionEntry {
    std::string id;
    std::string value;
  };
  std::vector<Epub3CollectionEntry> epub3Collections;
  std::vector<std::pair<std::string, std::string>> epub3GroupPositions;
  std::vector<std::pair<std::string, std::string>> epub3CollectionTypes;
  std::string activeMetaProperty;
  std::string activeMetaRefines;
  std::string activeMetaId;
  std::string activeMetaText;

  static constexpr uint16_t LARGE_SPINE_THRESHOLD = 400;

  // FNV-1a hash function
  static uint32_t fnvHash(const std::string& s) {
    uint32_t hash = 2166136261u;
    for (char c : s) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 16777619u;
    }
    return hash;
  }

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);
  void processEpub3Meta(const std::string& property, const std::string& refines, const std::string& id,
                        const std::string& value);
  void applyEpub3Metadata();

 public:
  std::string title;
  std::string author;
  std::string language;
  std::string series;
  std::string seriesIndex;
  std::string tocNcxPath;
  std::string tocNavPath;  // EPUB 3 nav document path
  std::string coverItemHref;
  std::string guideCoverPageHref;  // Guide reference with type="cover" or "cover-page" (points to XHTML wrapper)
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths

  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, const size_t xmlSize,
                            BookMetadataCache* cache)
      : cachePath(cachePath), baseContentPath(baseContentPath), remainingSize(xmlSize), cache(cache) {}
  ~ContentOpfParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
