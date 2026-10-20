#pragma once
#include <BufferedFileIO.h>
#include <Memory.h>     // makeUniqueNoThrow — nothrow growth (device is -fno-exceptions)
#include <PassTable.h>  // the spine idref table
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

  // Spine idrefs resolve a table-full at a time, one pass over .items.bin per table-full.
  //
  // They used to resolve one by one, through an in-RAM index of every manifest item (12 B an item,
  // grown by doubling) or, when the heap refused its next doubling, through a scan of .items.bin
  // per itemref. On the X3 King's Avatar's index stopped at 1024 items ("manifest index OOM at
  // 1024 items"; its ~3300 wanted 48 KB, plus the 24 KB it doubled from); a scan resumes after the
  // previous match, which reads the store about once for a spine in manifest order and about half
  // of it per itemref for any other order (a manifest in name order, as Sigil writes one).
  //
  // Now the itemrefs collect in a table (idref hash and length, place in the spine) until it is
  // full or the spine ends; one pass over the store, resuming after the last table-full's matches,
  // finds every href in it, and they go to the cache in spine order. The table is lent (a block of
  // the region first-open indexing lends, beside the stream's own: lendSpineTable), else the heap's
  // (a PassTable sized to the manifest's candidate items), else a single entry -- which is the old
  // resumed scan exactly. Nothing is indexed during the manifest: the parse holds no table of it.
  //
  // Matches trust (FNV-1a 64, length), as ZipFile's batch lookups and BookMetadataCache's spine href
  // index do; the first item in pass order with an idref's key is its href.
 public:
  struct SpineTarget {
    uint64_t idHash;
    uint16_t idLen;
    uint16_t slot;        // place in the table-full, in spine order
    uint32_t hrefOffset;  // of its href in .items.bin; NO_HREF until a pass finds it
  };
  static constexpr uint32_t NO_HREF = UINT32_MAX;
  static constexpr uint32_t EMITTED = UINT32_MAX - 1;  // written to the cache during the pass

 private:
  SpineTarget* spineTable_ = nullptr;
  size_t spineCapacity_ = 0;
  size_t spinePending_ = 0;
  size_t spinePasses_ = 0;
  bool spineTableLent_ = false;
  PassTable<SpineTarget> ownSpineTable_;
  SpineTarget inlineSpineTarget_ = {};
  // Manifest items a spine can name (not images, stylesheets or the NCX): the heap table's size.
  size_t spineCandidates_ = 0;
  // Where the next pass starts: the .items.bin offset just past the previous table-full's last
  // match, so a spine in manifest order reads the store about once in all.
  uint32_t itemScanResumeOffset_ = 0;
  std::string itemIdScratch_;
  std::string hrefScratch_;
  void beginSpineTable();
  void queueSpineItemRef(const char* idref);
  void resolvePendingSpineItemRefs();

  // Memo of the last manifest item's media-type and its classification (MediaClass enum in the
  // .cpp, stored as its uint8_t value here). Manifest items overwhelmingly repeat one media type
  // (all chapters share application/xhtml+xml, images share image/jpeg), so the common case is a
  // single strcmp hit instead of re-classifying — and the raw attribute is never copied into a
  // per-item std::string. Fixed buffer, zero heap; 48 covers every registered EPUB media type.
  char lastMediaType_[48] = {};
  uint8_t lastMediaClass_ = 0;

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

  static void startElement(void* userData, const char* name, const char** atts);
  static void characterData(void* userData, const char* s, int len);
  static void endElement(void* userData, const char* name);

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
  // Spine idrefs collect in `table` (capacity entries, not owned, valid until the parse ends)
  // instead of a heap table of the parser's own. Before the parse reaches <spine>.
  void lendSpineTable(SpineTarget* table, const size_t capacity) {
    spineTable_ = capacity > 0 ? table : nullptr;
    spineCapacity_ = spineTable_ != nullptr ? capacity : 0;
    spineTableLent_ = spineTable_ != nullptr;
  }
  // Hosts the SAX parser's state in `state` (SaxParser::stateBytes()) instead of the heap. Before setup().
  void setParserState(void* state) { saxParser_.setExternalState(state, SaxParser::stateBytes()); }
  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
