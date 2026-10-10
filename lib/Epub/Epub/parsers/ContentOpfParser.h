#pragma once
#include <Print.h>

#include <algorithm>
#include <deque>
#include <vector>

#include "Epub.h"
#include "expat.h"

class BookMetadataCache;

class ContentOpfParser final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_BOOK_TITLE,
    IN_BOOK_AUTHOR,
    IN_BOOK_LANGUAGE,
    IN_BOOK_IDENTIFIER,
    IN_BOOK_PUBLISHER,
    IN_BOOK_SUBJECT,
    IN_META_VALUE,
    IN_MANIFEST,
    IN_SPINE,
    IN_GUIDE,
  };

  enum class CollectionType : uint8_t { Untyped, Series, Other };

  static constexpr size_t MAX_COLLECTIONS = 4;
  static constexpr size_t MAX_REFINES = 8;

  struct AttributeFingerprint {
    uint32_t fnv = 0;
    uint32_t sdbm = 0;
    size_t length = 0;
    bool present = false;

    bool operator==(const AttributeFingerprint&) const = default;
  };

  struct StagedCollection {
    AttributeFingerprint id;
    std::string name;
  };
  struct StagedRefine {
    AttributeFingerprint target;
    std::string position;
    CollectionType type = CollectionType::Untyped;
  };

  const std::string& cachePath;
  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;
  const bool metadataOnly;
  bool metadataComplete = false;
  HalFile tempItemStore;
  AttributeFingerprint coverItemId;
  bool hasExplicitStartReference = false;
  // XML character data is allowed to arrive in several callbacks for one text
  // node (notably around character references). Keep whitespace and creator
  // separation as element state rather than inferring either from callbacks.
  bool metadataSpacePending = false;
  bool authorSeparatorPending = false;

  std::string identifierText;
  std::string identifierScheme;
  std::string metaText;
  AttributeFingerprint metaId;
  AttributeFingerprint metaRefines;
  bool metaIsCollection = false;
  bool metaIsCollectionType = false;

  StagedCollection collections[MAX_COLLECTIONS];
  size_t collectionCount = 0;
  StagedRefine refines[MAX_REFINES];
  size_t refineCount = 0;
  std::string calibreSeries;
  std::string calibreSeriesIndex;

  void resolveSeries();
  // Collect the first non-blank value of a dc: element
  void enterSingleValueElement(ParserState capturing, const std::string& value);
  // Resolve collection type and position by ID
  void resolveCollection(const AttributeFingerprint& id, CollectionType& type, std::string& position) const;

  // Index for fast idref→href lookup (binary search over .items.bin)
  struct ItemIndexEntry {
    uint32_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t fileOffset;  // offset in .items.bin
  };
  std::deque<ItemIndexEntry> itemIndex;
  bool useItemIndex = false;

  static AttributeFingerprint fingerprint(const char* value);

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

 public:
  std::string title;
  std::string author;
  std::string language;
  std::string isbn;
  std::string asin;
  std::string publisher;
  std::string subject;
  std::string tocNcxPath;
  std::string tocNavPath;  // EPUB 3 nav document path
  std::string coverItemHref;
  std::string guideCoverPageHref;  // Guide reference with type="cover" or "cover-page" (points to XHTML wrapper)
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths
  std::string series;
  std::string seriesIndexText;  // Original position text

  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, const size_t xmlSize,
                            BookMetadataCache* cache, const bool metadataOnly = false)
      : cachePath(cachePath),
        baseContentPath(baseContentPath),
        remainingSize(xmlSize),
        cache(cache),
        metadataOnly(metadataOnly) {}
  ~ContentOpfParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
