#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Epub.h"

class Page;
class GfxRenderer;
class ChapterHtmlSlimParser;
class CssParser;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  HalFile file;

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool embeddedStyle, uint8_t imageRendering, bool focusReadingEnabled);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

  // Page-offset table entry, kept in RAM while an incremental build is running so
  // already-built pages can be located in the partially-written .bin.
  struct PageLutEntry {
    uint32_t fileOffset;
    uint16_t paragraphIndex;
    uint16_t listItemIndex;
  };
  // Held only while an incremental build is in progress (see startBuild). Carries the
  // live parser plus the strings it references (the parser stores them by reference)
  // and the in-RAM page-offset table.
  struct BuildContext {
    std::unique_ptr<ChapterHtmlSlimParser> parser;
    std::vector<PageLutEntry> lut;
    std::string parsePath;
    std::string contentBase;
    std::string imageBasePath;
    std::string htmlPath;
    std::string tmpHtmlPath;
    bool reusedHtml = false;
    CssParser* cssParser = nullptr;
    // HTML byte progress, for estimating the section's total page count while it's still building.
    uint32_t bytesConsumed = 0;
    uint32_t totalBytes = 0;
    // Exponentially-smoothed page-count estimate (0 = not yet seeded) and the bytesConsumed at its
    // last update. The raw byte-ratio estimate jitters as the build crosses dense/sparse regions;
    // the EMA is stepped once per build advance (not per redraw) to damp that wobble.
    float smoothedEstimate = 0;
    uint32_t smoothedAtConsumed = 0;
  };
  std::unique_ptr<BuildContext> build_;
  bool buildComplete_ = false;
  bool finalizeBuild();

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  // Constructor and destructor are out-of-line: BuildContext holds a unique_ptr to the
  // forward-declared ChapterHtmlSlimParser, whose full definition is only visible in the .cpp.
  explicit Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer);
  ~Section();
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                       uint8_t imageRendering, bool focusReadingEnabled);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                         uint8_t imageRendering, bool focusReadingEnabled,
                         const std::function<void()>& popupFn = nullptr);

  // Incremental build: lay out the section a few pages at a time so a large chapter
  // can show its first page immediately and keep the UI responsive while the rest
  // builds. createSectionFile() above is the one-shot wrapper over these.
  //   if (!startBuild(...)) fail;
  //   each tick: buildSomeMore(N); render up to pageCount; when isBuildComplete() stop.
  bool startBuild(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                  uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                  uint8_t imageRendering, bool focusReadingEnabled, const std::function<void()>& popupFn = nullptr);
  // Lay out up to maxPages more pages (maxPages <= 0 = build to completion). Returns
  // false on error (the build is abandoned). Sets isBuildComplete() when finished.
  bool buildSomeMore(int maxPages);
  bool isBuilding() const { return static_cast<bool>(build_); }
  bool isBuildComplete() const { return buildComplete_; }
  // Best-known total page count: the exact pageCount once finalized, or a smoothed byte-based
  // estimate (pages so far scaled by totalBytes/bytesConsumed, damped by an EMA) while a giant spine
  // is still building, so "page X of Y" / progress don't read off the small build watermark.
  uint16_t estimatedTotalPages() const;
  void abandonBuild();
  // Read a page already laid out by the in-progress build (page < pageCount), from
  // the partially-written .bin without disturbing the build's write cursor.
  std::unique_ptr<Page> loadPageDuringBuild(int page);

  std::unique_ptr<Page> loadPageFromSectionFile();
  std::string getTextFromSectionFile();

  // True if this spine's unzipped HTML is already cached, so a build won't pay the (multi-second on a
  // giant spine) zip inflation. Lets the reader skip the indexing popup on a fast reopen/rebuild.
  bool hasHtmlCache() const;

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Look up an anchor among the pages built so far by the in-progress build, so an anchor jump
  // (TOC / chapter select, usually the chapter top = page 0) can resolve without laying out the
  // whole chapter. Returns nullopt if the anchor hasn't been reached yet (build more) or no build.
  std::optional<uint16_t> findAnchorDuringBuild(const std::string& anchor) const;

  // Get the page count from the section cache file without fully loading it.
  std::optional<uint16_t> getCachedPageCount() const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the page number for a running list-item index from the li LUT.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;
};
