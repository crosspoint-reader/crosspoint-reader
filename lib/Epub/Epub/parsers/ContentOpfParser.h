#pragma once
#include <Print.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
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
    IN_BOOK_DESCRIPTION,
    IN_META_TEXT,
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
  const bool metadataOnly;
  bool metadataComplete = false;
  HalFile tempItemStore;
  std::string coverItemId;
  bool hasExplicitStartReference = false;
  // XML character data is allowed to arrive in several callbacks for one text
  // node (notably around character references). Keep whitespace and creator
  // separation as element state rather than inferring either from callbacks.
  bool metadataSpacePending = false;
  bool authorSeparatorPending = false;
  // Set once a non-blank description is kept or the cap cut it.
  bool descriptionDone = false;
  // The description holds raw XHTML elements, so its text must be re-escaped.
  bool descriptionXhtml = false;

  std::string identifierText;
  std::string identifierScheme;
  std::string metaText;
  std::string metaProperty;
  std::string metaRefines;
  std::string metaId;
  struct CollectionMetadata {
    std::string id;
    std::string title;
    std::optional<float> index;
    bool isSeries = false;
  };
  std::vector<CollectionMetadata> collectionCandidates;
  std::string calibreSeries;
  std::optional<float> calibreSeriesIndex;

  // Index for fast idref→href lookup (binary search over .items.bin).
  //
  // Bounded, nothrow-growable storage: a manifest can contain thousands of
  // items (e.g. an image-heavy encyclopaedia EPUB has one <item> per
  // thumbnail), and only a subset of those are ever targets of a spine
  // itemref. A throwing container (std::deque/std::vector push_back) sized
  // 1:1 with manifest items was observed to exhaust heap and abort() via
  // bad_alloc under -fno-exceptions on such books. This index is therefore:
  //   1. Filtered at insertion (see startElement): items whose media-type
  //      can never be a spine target (image/*, CSS, NCX) are not indexed.
  //   2. Hard-capped at ITEM_INDEX_MAX_ENTRIES with nothrow doubling growth.
  // Because the index is intentionally a *subset* of manifest items, every
  // idref lookup that misses the index falls back to a linear scan of
  // .items.bin (see startElement, IN_SPINE branch) — correctness never
  // depends on every item being indexed, only performance does.
  struct ItemIndexEntry {
    uint32_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t fileOffset;  // offset in .items.bin
  };
  static constexpr size_t ITEM_INDEX_INITIAL_CAPACITY = 64;
  // ~36KB worst case (12 bytes/entry). Well above realistic "document" item
  // counts (spine-referenceable items), since image/CSS/NCX items are
  // filtered out before ever reaching the index.
  static constexpr size_t ITEM_INDEX_MAX_ENTRIES = 3000;
  std::unique_ptr<ItemIndexEntry[]> itemIndex;
  size_t itemIndexCount = 0;
  size_t itemIndexCapacity = 0;
  bool itemIndexOverflowed = false;
  bool useItemIndex = false;

  // Appends to the bounded index with nothrow doubling growth. Returns false
  // (without indexing the entry) once ITEM_INDEX_MAX_ENTRIES is reached or on
  // OOM; callers must tolerate misses via the linear-scan fallback.
  bool pushIndexEntry(const ItemIndexEntry& entry);

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
  std::string series;
  std::optional<float> seriesIndex;
  // First <dc:description>, raw (often escaped HTML). Only collected when
  // descriptionLimit is set, up to that many bytes.
  std::string description;
  size_t descriptionLimit = 0;
  std::string tocNcxPath;
  std::string tocNavPath;  // EPUB 3 nav document path
  std::string coverItemHref;
  std::string guideCoverPageHref;  // Guide reference with type="cover" or "cover-page" (points to XHTML wrapper)
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths

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
