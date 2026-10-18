#pragma once
#include <BufferedFileIO.h>
#include <Memory.h>  // makeUniqueNoThrow — nothrow growth (device is -fno-exceptions)
#include <Print.h>
#include <SaxParser/SaxParser.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "Epub.h"

class BookMetadataCache;

class ContentOpfParser final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_BOOK_TITLE,
    IN_BOOK_AUTHOR,
    IN_BOOK_LANGUAGE,
    IN_BOOK_DESCRIPTION,
    IN_BOOK_SERIES,
    IN_BOOK_SERIES_INDEX,
    IN_BOOK_IDENTIFIER,
    IN_CREATOR_REFINE,
    IN_MANIFEST,
    IN_SPINE,
    IN_GUIDE,
  };

  const std::string& cachePath;
  const std::string& baseContentPath;
  size_t remainingSize;
  SaxParser saxParser_;
  ParserState state = START;
  BookMetadataCache* cache;
  FsFile tempItemStore;
  // Buffered views over tempItemStore, alive one phase at a time: the writer during the
  // manifest pass (thousands of tiny id/href field writes), the reader during the spine pass
  // (one seek + string read per itemref, usually landing inside the current 4 KB window since
  // spine order tracks manifest order). Each internally degrades to pass-through on OOM.
  std::optional<serialization::BufferedFileWriter> itemWriter_;
  std::optional<serialization::BufferedFileReader> itemReader_;
  std::string coverItemId;

  // Index for fast idref→href lookup (used only for large EPUBs).
  // Lookups trust (idHash, idLen) alone and read the href directly — the id string is never
  // read back for verification. Hash-trusted matching adapted from the FreeInk SDK's
  // BookCatalog (github.com/Free-Ink/freeink-sdk, "Add SD-backed catalog for large EPUB
  // containers" by Justin Mitchell), whose rationale applies verbatim: the strings a verify
  // would need are exactly the RAM/IO this index exists to avoid. Correctness is guaranteed
  // by a duplicate scan after the sort: if any two manifest ids share (hash, len), the index
  // is disabled for the whole book and lookups fall back to the exact linear scan.
  struct ItemIndexEntry {
    uint32_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t hrefOffset;  // offset of the href string in .items.bin (record's id skipped)
  };
  // In-RAM manifest fast index (sorted by (idHash,idLen) for binary-search idref lookup). Grown with
  // NOTHROW allocation (device is -fno-exceptions, so a throwing container abort()s on a fragmented
  // heap — observed on a 1732-spine book). It grows as long as memory allows; the FIRST growth that
  // can't be satisfied latches indexDisabled_ and idref lookups fall back to the exact linear scan
  // over .items.bin. So a big book keeps the fast index whenever the heap can hold it (~12 bytes/item),
  // and only genuinely-out-of-memory books degrade — no arbitrary item-count cap. The index is a pure
  // speed optimisation, never correctness. push_back returns false on OOM (never throws).
  struct ItemIndexVec {
    std::unique_ptr<ItemIndexEntry[]> data_;
    size_t size_ = 0;
    size_t cap_ = 0;
    bool push_back(const ItemIndexEntry& e) {
      if (size_ == cap_) {
        const size_t newCap = cap_ == 0 ? 64 : cap_ * 2;
        auto next = makeUniqueNoThrow<ItemIndexEntry[]>(newCap);
        if (!next) return false;  // OOM — caller disables the index, falls back to linear scan
        if (size_ > 0) std::memcpy(next.get(), data_.get(), size_ * sizeof(ItemIndexEntry));
        data_ = std::move(next);
        cap_ = newCap;
      }
      data_[size_++] = e;
      return true;
    }
    void clear() {
      data_.reset();
      size_ = 0;
      cap_ = 0;
    }
    size_t size() const { return size_; }
    ItemIndexEntry* begin() { return data_.get(); }
    ItemIndexEntry* end() { return data_.get() + size_; }
    const ItemIndexEntry& operator[](size_t i) const { return data_[i]; }
  };
  ItemIndexVec itemIndex;
  bool useItemIndex = false;
  bool indexDisabled_ = false;  // latched when an index growth hit OOM → linear-scan fallback
  // Where the linear scan picks up: the .items.bin offset just past its previous match (see
  // resolveItemRefHrefLinearScan).
  uint32_t itemScanResumeOffset_ = 0;

  // Memo of the last manifest item's media-type and its classification (MediaClass enum in the
  // .cpp, stored as its uint8_t value here). Manifest items overwhelmingly repeat one media type
  // (all chapters share application/xhtml+xml, images share image/jpeg), so the common case is a
  // single strcmp hit instead of re-classifying — and the raw attribute is never copied into a
  // per-item std::string. Fixed buffer, zero heap; 48 covers every registered EPUB media type.
  char lastMediaType_[48] = {};
  uint8_t lastMediaClass_ = 0;

  static constexpr uint16_t LARGE_SPINE_THRESHOLD = 400;

  // One dc:creator as the <metadata> block describes it. Kept only until the block ends, to pick
  // the primary author then: EPUB 3 states a creator's role and filing name in <meta refines="#id">
  // elements that come after it.
  struct Creator {
    std::string id;
    std::string name;
    std::string role;
    std::string fileAs;
  };
  // The first few creators are enough: books credit their authors first. Allocated from the heap,
  // and only once a dc:creator appears, because the parser itself lives on the caller's stack.
  static constexpr uint8_t MAX_CREATORS = 4;
  // Longest creator name, id, role or filing name kept.
  static constexpr size_t MAX_CREATOR_FIELD = 256;
  std::unique_ptr<Creator[]> creators_;
  uint8_t creatorCount_ = 0;
  Creator* currentCreator_ = nullptr;    // the dc:creator being read; null past MAX_CREATORS or on OOM
  std::string creatorText_;              // its text, gathered across however many pieces it arrives in
  std::string* refineTarget_ = nullptr;  // the creator field a <meta refines> element's text fills

  Creator* beginCreator(const char** atts);
  std::string* creatorField(const char* id, const char* property);
  void pickPrimaryAuthor();

  // The dc:identifier being read: what its scheme attribute says it is, whether it is the package's
  // unique identifier, and its text. Longer than any ISBN or ASIN can be means it is neither, and a
  // cut-off identifier would be a wrong one, so one longer than MAX_IDENTIFIER is not kept at all.
  enum class IdentifierScheme : uint8_t { Other, Isbn, Asin };
  static constexpr size_t MAX_ISBN_OR_ASIN = 64;
  static constexpr size_t MAX_IDENTIFIER = 256;
  IdentifierScheme identifierScheme_ = IdentifierScheme::Other;
  std::string identifierText_;
  bool identifierTooLong_ = false;
  bool identifierIsUnique_ = false;
  std::string uniqueIdentifierId_;  // the package's unique-identifier attribute
  void recordIdentifier();
  bool metadataRead_ = false;

  // FNV-1a hash function
  static uint32_t fnvHash(const std::string& s) {
    uint32_t hash = 2166136261u;
    for (char c : s) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 16777619u;
    }
    return hash;
  }

  static void startElement(void* userData, const char* name, const char** atts);
  static void characterData(void* userData, const char* s, int len);
  static void endElement(void* userData, const char* name);

  bool resolveItemRefHrefWithIndex(const std::string& idref, std::string& href);
  bool resolveItemRefHrefLinearScan(const std::string& idref, std::string& href);
  bool resolveSpineItemRefHref(const std::string& idref, std::string& href);
  // Closes tempItemStore if it was ever opened — a null-cache parse never opens it.
  void closeItemStore();
  void handleSpineItemRefElement(const char** atts);

 public:
  struct Stats {
    uint32_t writeCalls = 0;
    uint32_t bytesParsed = 0;
    uint32_t parseBufferMs = 0;
    uint32_t manifestOpenMs = 0;
    uint32_t spineOpenMs = 0;
    uint32_t guideOpenMs = 0;
    uint32_t itemRefCount = 0;
    uint32_t itemRefLookupMs = 0;
    uint32_t createSpineEntryMs = 0;
  } stats;

  std::string title;
  // Every dc:creator's text, joined with ", ": what the book shows as its author.
  std::string author;
  // The first dc:creator credited as the book's author (role "aut", or no role at all), and how it
  // files (opf:file-as, or EPUB 3 <meta refines property="file-as">). For grouping and ordering by
  // author; `author` stays the display line. Empty when no creator qualifies.
  std::string primaryAuthor;
  std::string authorSort;
  std::string language;
  std::string description;
  std::string series;
  std::string seriesIndex;
  // The first dc:identifier that is an ISBN, and the first that is an Amazon ASIN, without their
  // "urn:isbn:" / "isbn:" prefixes. For KOReader sync's metadata; empty when the book gives none.
  std::string isbn;
  std::string asin;
  // The package's unique identifier (the dc:identifier its unique-identifier attribute names, else
  // the first) and its first urn:uuid: identifier, trimmed: the keys of a book's obfuscated fonts
  // (BookFonts). Empty when the book gives none.
  std::string uniqueIdentifier;
  std::string uuidIdentifier;
  std::string tocNcxPath;
  std::string tocNavPath;   // EPUB 3 nav document path
  std::string pageMapPath;  // EPUB 2.01 page-map.xml document path
  std::string coverItemHref;
  std::string guideCoverPageHref;  // Guide reference with type="cover" or "cover-page" (points to XHTML wrapper)
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths

  // Out of line: three callers, and the members it initialises are many.
  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, size_t xmlSize,
                            BookMetadataCache* cache);
  ~ContentOpfParser() override;

  // Set once </metadata> has closed: the stop flag of a read that wants the metadata alone (BookFonts' keys), so
  // a large manifest is not parsed for nothing.
  const bool* metadataRead() const { return &metadataRead_; }
  // Hosts the SAX parser's state in `state` (SaxParser::stateBytes()) instead of the heap. Before setup().
  void setParserState(void* state) { saxParser_.setExternalState(state, SaxParser::stateBytes()); }
  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
