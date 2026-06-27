#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "Epub.h"

class Page;
class GfxRenderer;

class Section {
  std::shared_ptr<Epub> epub;
  int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  // Byte length of the constant "<cachePath>/sections/" prefix within filePath.
  // resetForSpine() truncates filePath to this length and re-appends only the
  // numeric suffix, reusing the buffer instead of allocating a new string per
  // spine transition.
  size_t sectionPathPrefixLen = 0;
  HalFile file;

  // Cached section-header state for the search scan: the file size and page-LUT
  // offset are invariant per section, so they are read once when the scan file
  // is lazily opened and reused for every pageContainsText() call. Invalidated
  // by resetForSpine() (which also closes the file).
  bool searchHeaderReady = false;
  uint32_t searchFileSize = 0;
  uint32_t searchLutOffset = 0;

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool embeddedStyle, uint8_t imageRendering, bool focusReadingEnabled);
  uint32_t onPageComplete(std::unique_ptr<Page> page, uint32_t& searchTextOffset);
  // Seek to and read the page-LUT offset from the section header (the single
  // place that knows where that field lives). Requires the member file to be
  // open; returns false on seek/read failure.
  bool readPageLutOffset(uint32_t& lutOffset);
  // Lazily open the scan file and cache its size and page-LUT offset. Returns
  // false on open failure or a truncated/corrupt header.
  bool ensureSearchHeader();
  // Rewrite filePath's numeric suffix in place for the current spineIndex,
  // reusing the buffer (no per-spine string allocation, no std::to_string).
  void rebuildFilePathForSpine();

 public:
  static constexpr size_t MAX_SEARCH_QUERY_BYTES = 64;
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
      : epub(epub), spineIndex(spineIndex), renderer(renderer) {
    // Build the constant "<cachePath>/sections/" prefix once and remember its
    // length; resetForSpine() then rewrites only the numeric suffix in place.
    const std::string& cachePath = this->epub->getCachePath();
    filePath.reserve(cachePath.size() + 32);  // prefix + up to 11 digits + ".bin"
    filePath.assign(cachePath).append("/sections/");
    sectionPathPrefixLen = filePath.size();
    rebuildFilePathForSpine();
  }
  ~Section() = default;
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                       uint8_t imageRendering, bool focusReadingEnabled);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                         uint8_t imageRendering, bool focusReadingEnabled,
                         const std::function<void()>& popupFn = nullptr);
  std::unique_ptr<Page> loadPageFromSectionFile();
  std::string getTextFromSectionFile();

  // Reuse this Section object for another spine item without another heap
  // allocation. Intended for sequential, book-wide operations such as search.
  void resetForSpine(int newSpineIndex);

  // Single source of truth for whether a query is usable for search: non-empty,
  // not all-whitespace, and within the byte limit. The UI validates with this
  // before launching a search.
  static bool isValidSearchQuery(std::string_view query);

  // A query compiled once for a whole-book search: the normalized pattern
  // (lowercase, spaces and hyphens dropped), its length, and the KMP failure
  // table over it. Built by compileSearchQuery(), consumed by pageContainsText().
  struct CompiledSearchQuery {
    std::array<uint8_t, MAX_SEARCH_QUERY_BYTES> pattern{};
    std::array<uint8_t, MAX_SEARCH_QUERY_BYTES> prefix{};
    size_t length = 0;
  };

  // Normalize a query for matching: fold ASCII A-Z to lowercase and drop ASCII
  // spaces and hyphens (so layout-time hyphenation and spacing differences do
  // not block a match). Writes the normalized bytes into `out` and returns the
  // normalized length.
  static size_t normalizeSearchQuery(std::string_view query, std::array<uint8_t, MAX_SEARCH_QUERY_BYTES>& out);

  // Compile a query once for a book-wide search: normalize it and build the KMP
  // failure table over the result, so every page scan reuses one consistent
  // pattern + table. Returns false for an empty/oversized query or one that
  // normalizes to nothing (e.g. only spaces or hyphens).
  static bool compileSearchQuery(std::string_view query, CompiledSearchQuery& out);

  // Streams the compact text record for one page through a fixed-size buffer,
  // matching the compiled query while skipping spaces and hyphens in the record.
  // `matched` is the KMP partial-match length carried in and out: pass the value
  // left by the previous adjacent page so a query split across a page boundary
  // (e.g. a line-hyphenated word, "inter-" then "national") still matches; the
  // caller must reset it to 0 at any reading-order discontinuity (scan start,
  // spine change, wrap). An empty record resets it. nullopt indicates an
  // invalid/corrupt cache record; false is a valid miss.
  std::optional<bool> pageContainsText(uint16_t page, const CompiledSearchQuery& query, size_t& matched);

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Get the page count from the section cache file without fully loading it.
  std::optional<uint16_t> getCachedPageCount() const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the page number for a running list-item index from the li LUT.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;
};
