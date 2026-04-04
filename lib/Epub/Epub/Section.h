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

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool embeddedStyle, uint8_t imageRendering);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

  struct TocBoundary {
    int tocIndex = 0;
    uint16_t startPage = 0;
  };
  std::vector<TocBoundary> tocBoundaries;

  void buildTocBoundaries(const std::vector<std::pair<std::string, uint16_t>>& anchors);
  void buildTocBoundariesFromFile(FsFile& f);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}
  ~Section() = default;
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                       uint8_t imageRendering);
  bool clearCache();
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                         uint8_t imageRendering, const std::function<void(int)>& progressFn = nullptr);
  std::unique_ptr<Page> loadPageFromSectionFile();

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

  // Look up the page number for a paragraph index (1-based, from XPath p[N]).
  // Uses the per-page paragraph index LUT stored in the section cache.
  // Returns nullopt if the paragraph LUT is not available (old cache format).
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the paragraph index for a given page number.
  // Returns the 1-based paragraph index of the last <p> element on or before the page.
  // Returns nullopt if the paragraph LUT is not available (old cache format).
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;
};
