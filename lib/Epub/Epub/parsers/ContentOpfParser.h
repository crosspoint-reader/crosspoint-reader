#pragma once
#include <Print.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
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
    IN_BOOK_SUBJECT,
    IN_META_TEXT,  // EPUB 3 <meta property="..."> whose text content is kept
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

  // Index for fast idref→href lookup (binary search over .items.bin)
  struct ItemIndexEntry {
    uint32_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t fileOffset;  // offset in .items.bin
  };
  std::deque<ItemIndexEntry> itemIndex;
  bool useItemIndex = false;

  // Working state for sort keys, series and tags: creator/collection ids and
  // EPUB 3 refinements that can only be resolved once <metadata> closes.
  // Heap-allocated in setup() and freed at the end of <metadata>, because the
  // parser itself lives on the caller's stack.
  struct ExtendedState;
  std::unique_ptr<ExtendedState> ext;

  void addTag(std::string& value);
  void endMetaText();
  void finalizeExtendedMetadata();

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
  // Metadata text has its whitespace collapsed to single spaces, so a newline
  // can never occur inside a tag.
  static constexpr char TAG_SEPARATOR = '\n';

  std::string title;
  std::string author;
  std::string language;
  // Sort keys, series and tags; each is empty when the OPF does not provide it.
  std::string titleSort;
  std::string authorSort;  // file-as of the first creator whose role is "aut" or unset
  std::string series;
  std::string seriesIndex;
  std::string tags;  // TAG_SEPARATOR-joined, deduplicated case-insensitively
  std::string tocNcxPath;
  std::string tocNavPath;  // EPUB 3 nav document path
  std::string coverItemHref;
  std::string guideCoverPageHref;  // Guide reference with type="cover" or "cover-page" (points to XHTML wrapper)
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths

  // Out of line so ExtendedState is complete wherever `ext` may be destroyed.
  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, size_t xmlSize,
                            BookMetadataCache* cache, bool metadataOnly = false);
  ~ContentOpfParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
