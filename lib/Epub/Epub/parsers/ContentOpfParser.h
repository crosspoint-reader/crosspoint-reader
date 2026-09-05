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
    IN_META_VALUE,
    IN_MANIFEST,
    IN_SPINE,
    IN_GUIDE,
  };

  // What a collection's refines say it is. Untyped is the ordinary case: most
  // documents that name a collection never state its type.
  enum class CollectionType : uint8_t { Untyped, Series, Other };

  // A document may name several collections, and a boxed set alongside the
  // series it belongs to is the ordinary pairing rather than an exotic one. All
  // are staged so the series-typed one can be preferred; keeping only the first
  // files a book under its box set, or loses the series when the set comes
  // first. Four is past anything seen in the wild, and the cap is what keeps
  // this bounded.
  static constexpr size_t MAX_COLLECTIONS = 4;
  // Refines may precede the collection they describe, so they are staged too.
  static constexpr size_t MAX_REFINES = 8;

  struct StagedCollection {
    std::string id;
    std::string name;
  };
  struct StagedRefine {
    std::string target;
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
  std::string coverItemId;
  bool hasExplicitStartReference = false;
  // XML character data is allowed to arrive in several callbacks for one text
  // node (notably around character references). Keep whitespace and creator
  // separation as element state rather than inferring either from callbacks.
  bool metadataSpacePending = false;
  bool authorSeparatorPending = false;

  // Text of the <meta> element being read, and what to do with it on the
  // closing tag. Element content is only known once the whole element is seen,
  // so the attributes that decide its meaning are captured on the opening tag.
  std::string metaText;
  std::string metaId;
  std::string metaRefines;
  bool metaIsCollection = false;
  bool metaIsCollectionType = false;
  bool metaIsGroupPosition = false;

  StagedCollection collections[MAX_COLLECTIONS];
  size_t collectionCount = 0;
  StagedRefine refines[MAX_REFINES];
  size_t refineCount = 0;
  // Calibre's own series tags, which win over belongs-to-collection when a book
  // carries both: that value is the one a reader curated by hand.
  std::string calibreSeries;
  std::string calibreSeriesIndex;

  // Pick the series from everything staged during <metadata>.
  void resolveSeries();
  // Read the collection-type and group-position refining `id`. A collection with
  // no id carries no refines to find, so it stays untyped and positionless.
  void resolveCollection(const std::string& id, CollectionType& type, std::string& position) const;

  // Index for fast idref→href lookup (binary search over .items.bin)
  struct ItemIndexEntry {
    uint32_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t fileOffset;  // offset in .items.bin
  };
  std::deque<ItemIndexEntry> itemIndex;
  bool useItemIndex = false;

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
  std::string tocNcxPath;
  std::string tocNavPath;  // EPUB 3 nav document path
  std::string coverItemHref;
  std::string guideCoverPageHref;  // Guide reference with type="cover" or "cover-page" (points to XHTML wrapper)
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths
  // Series the book belongs to, and its position as the document writes it
  // ("3", "3.5", "0.5"). The position is left as text because how it is encoded
  // is the Library index's business, not the parser's.
  std::string series;
  std::string seriesIndexText;

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
