#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Epub.h"

class Page;
class GfxRenderer;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  FsFile file;
  std::vector<uint32_t> lut;  // Cached page byte-offsets; loaded once, avoids per-page LUT seek
  bool truncatedCache = false;
  bool embeddedStyleFallback = false;

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool embeddedStyle, bool bionicReadingEnabled, uint8_t imageRendering);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

  struct TocBoundary {
    int tocIndex = 0;
    uint16_t startPage = 0;
  };
  std::vector<TocBoundary> tocBoundaries;
  std::vector<std::pair<uint16_t, std::string>> pageBreakLabels;

  void buildTocBoundaries(const std::vector<std::pair<std::string, uint16_t>>& anchors);
  void buildTocBoundariesFromFile(FsFile& f);
  void buildPageBreakLabelsFromFile(FsFile& f);

  // Open the section file and seek to the first paragraph LUT entry, validating the header
  // and LUT bounds against fileSize. On success, returns true with `outLutStart` set to the
  // byte offset of the first entry (just past the count) and `outCount` to the entry count.
  // Caller is responsible for closing `outFile`. Returns false on any I/O or validation error.
  bool readParagraphLutHeader(FsFile& outFile, uint16_t& outCount, uint32_t& outLutStart) const;

  // Calculates a stable hash for a given set of rendering properties.
  // Used to suffix cache files so multiple variants can coexist safely without constant recompilation.
  static uint32_t calculatePropertyHash(int fontId, float lineCompression, bool extraParagraphSpacing,
                                        uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                                        bool hyphenationEnabled, bool embeddedStyle, bool bionicReadingEnabled,
                                        uint8_t imageRendering);

  // Computes the active file path for this section based on rendering properties
  std::string getSectionFilePath(uint32_t propertyHash) const;
  // Computes the image base path for extract images related to this specific section variant
  std::string getImageBasePath(uint32_t propertyHash) const;
  // Garbage collection: Keep only the most recent N variants per chapter
  void evictOldVariants() const;

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
      : epub(epub), spineIndex(spineIndex), renderer(renderer) {}
  ~Section() = default;
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                       bool bionicReadingEnabled, uint8_t imageRendering);
  bool clearCache();
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                         bool bionicReadingEnabled, uint8_t imageRendering,
                         const std::function<void(int)>& progressFn = nullptr, bool skipEviction = false);
  std::unique_ptr<Page> loadPageFromSectionFile();
  bool isTruncatedCache() const { return truncatedCache; }
  bool isEmbeddedStyleFallback() const { return embeddedStyleFallback; }

  // Given a page in this section, return the TOC index for that page.
  int getTocIndexForPage(int page) const;
  // Given a TOC index, return the start page in this section.
  // Returns nullopt if the TOC index doesn't map to a boundary in this spine (e.g. belongs to a different spine).
  std::optional<int> getPageForTocIndex(int tocIndex) const;

  struct TocPageRange {
    int startPage;  // inclusive
    int endPage;    // exclusive
  };
  // Returns the page range [start, end) within this spine that belongs to the given TOC index.
  std::optional<TocPageRange> getPageRangeForTocIndex(int tocIndex) const;

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;
  // Returns the printed-page label for a rendered page, wrapped in parens (e.g. "(42)"), if
  // one or more EPUB pagebreak markers / NCX <pageList> / page-map entries land on it.
  // Returns nullopt when no printed-page anchor falls on this exact page.
  std::optional<std::string> getPrintedPageLabelForPage(uint16_t page) const;
  // Like getPrintedPageLabelForPage but returns the most recent printed-page label at or
  // before `page` (raw label, no parens). Useful for pre-filling jump-to-page dialogs when
  // the current rendered page doesn't itself carry an anchor. Returns nullopt when no
  // printed-page anchor exists on this or any earlier page in the section.
  std::optional<std::string> getNearestPrintedPageLabelAtOrBefore(uint16_t page) const;

  // Standalone lookup that doesn't require a loaded Section. Walks the book's sections cache
  // directory, finds any cache variant for `spineIndex`, reads its printed-page label map,
  // and returns the parenthesised label for `page` if one is recorded. Returns nullopt when
  // no cache exists or the page carries no printed-page anchor. Used by SleepActivity to
  // augment the overlay without instantiating a full Section + render parameters.
  static std::optional<std::string> getPrintedPageLabelFromCache(const std::string& sectionsDir, int spineIndex,
                                                                 uint16_t page);

  // Look up the page number for a paragraph index (1-based, from XPath p[N]).
  // Uses the per-page paragraph LUT stored in the section cache.
  // Returns nullopt if the paragraph LUT is not available (old cache format).
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the page number for a running <li> index (1-based, the Nth <li> at any depth
  // in the chapter). Used to snap KOReader-supplied list-item XPaths to a precise page
  // the same way getPageForParagraphIndex handles <p>-anchored XPaths.
  // Returns nullopt if the LUT is not available or the index is out of range.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the paragraph index for a given page number.
  // Returns the 1-based paragraph index of the last <p> element on or before the page.
  // Returns nullopt if the paragraph LUT is not available (old cache format).
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;

  // Look up the XHTML byte offset recorded at the page break that started the given page.
  // This is the Expat byte position within the decompressed spine XHTML file — useful as a
  // seek hint for findXPathForParagraph to avoid scanning from byte 0 on large chapters.
  // Returns nullopt if the paragraph LUT is unavailable (old cache format) or offset is 0
  // (last page, recorded after parse completion).
  std::optional<uint32_t> getXhtmlByteOffsetForPage(uint16_t page) const;
};
