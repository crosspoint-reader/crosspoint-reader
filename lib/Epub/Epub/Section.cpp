#include "Section.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint8_t SECTION_FILE_VERSION = 23;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) +   // SECTION_FILE_VERSION
                                 sizeof(int) +       // fontId
                                 sizeof(float) +     // lineCompression
                                 sizeof(bool) +      // extraParagraphSpacing
                                 sizeof(uint8_t) +   // paragraphAlignment
                                 sizeof(uint16_t) +  // viewportWidth
                                 sizeof(uint16_t) +  // viewportHeight
                                 sizeof(uint16_t) +  // pageCount (stored as 16-bit in header)
                                 sizeof(bool) +      // hyphenationEnabled
                                 sizeof(bool) +      // embeddedStyle
                                 sizeof(uint8_t) +   // imageRendering
                                 sizeof(uint32_t) +  // page LUT offset
                                 sizeof(uint32_t) +  // anchor map offset
                                 sizeof(uint32_t);   // paragraph LUT offset

// On-disk paragraph LUT entry: u32 xhtmlByteOffset + u16 paragraphIndex.
constexpr uint32_t PARAGRAPH_LUT_ENTRY_SIZE = sizeof(uint32_t) + sizeof(uint16_t);
inline uint32_t paragraphLutEntryOffset(uint32_t lutStart, uint16_t page) {
  return lutStart + page * PARAGRAPH_LUT_ENTRY_SIZE;
}
}  // namespace

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", pageCount);

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                     const uint16_t viewportHeight, const bool hyphenationEnabled,
                                     const bool embeddedStyle, const uint8_t imageRendering) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(embeddedStyle) + sizeof(imageRendering) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, embeddedStyle);
  serialization::writePod(file, imageRendering);
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for paragraph LUT offset (patched later)
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                              const uint8_t imageRendering) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  {
    uint8_t version;
    serialization::readPod(file, version);
    if (version != SECTION_FILE_VERSION) {
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();  // closes file before removal
      return false;
    }

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileEmbeddedStyle);
    serialization::readPod(file, fileImageRendering);

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
        viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
        hyphenationEnabled != fileHyphenationEnabled || embeddedStyle != fileEmbeddedStyle ||
        imageRendering != fileImageRendering) {
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();  // closes file before removal
      return false;
    }
  }

  serialization::readPod(file, pageCount);

  // Sanity check: same upper bound used by TextBlock::deserialize for word count
  if (pageCount > 10000) {
    LOG_ERR("SCT", "Deserialization failed: page count %u exceeds maximum", pageCount);
    clearCache();
    return false;
  }

  // Load LUT into memory (file is now positioned at the lutOffset field)
  uint32_t lutOffset;
  serialization::readPod(file, lutOffset);
  lut.resize(pageCount);
  if (!file.seek(lutOffset)) {
    LOG_ERR("SCT", "Deserialization failed: seek to LUT offset %u failed", lutOffset);
    clearCache();
    return false;
  }
  for (uint32_t& pos : lut) {
    serialization::readPod(file, pos);
    if (pos < HEADER_SIZE || pos >= lutOffset) {
      LOG_ERR("SCT", "Deserialization failed: LUT entry %u out of range [%u, %u)", pos, HEADER_SIZE, lutOffset);
      clearCache();
      return false;
    }
  }
  // Build TOC boundaries by scanning anchor data from the still-open file,
  // matching only the TOC anchors we need (avoids loading all anchors into memory).
  buildTocBoundariesFromFile(file);

  // File is intentionally left open; subsequent loadPageFromSectionFile() calls
  // seek within this handle instead of re-opening the file each time.
  LOG_DBG("SCT", "Deserialization succeeded: %d pages, LUT cached", pageCount);
  return true;
}

bool Section::clearCache() {
  file.close();  // Must be closed before removal on FAT32
  lut.clear();
  pageCount = 0;
  currentPage = 0;

  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                                const uint8_t imageRendering, const std::function<void(int)>& progressFn) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Retry logic for SD card timing issues
  bool success = false;
  uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      delay(50);  // Brief delay before retry
    }

    // Remove any incomplete file from previous attempt before retrying
    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }

    FsFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    tmpHtml.close();

    // If streaming failed, remove the incomplete file immediately
    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }

  if (!success) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }

  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes)", tmpHtmlPath.c_str(), fileSize);

  if (!Storage.openFileForWrite("SCT", filePath, file)) {
    return false;
  }
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering);
  std::vector<uint32_t> lut = {};

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  CssParser* cssParser = nullptr;
  if (embeddedStyle) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      if (!cssParser->loadFromCache()) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries
  std::vector<std::string> tocAnchors;
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
      viewportHeight, hyphenationEnabled,
      [this, &lut](std::unique_ptr<Page> page) { lut.emplace_back(this->onPageComplete(std::move(page))); },
      embeddedStyle, contentBase, imageBasePath, imageRendering, std::move(tocAnchors), progressFn, cssParser);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  success = visitor.parseAndBuildPages();

  Storage.remove(tmpHtmlPath.c_str());
  if (!success) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    file.close();
    Storage.remove(filePath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT
  for (const uint32_t& pos : lut) {
    if (pos == 0) {
      hasFailedLutRecords = true;
      break;
    }
    serialization::writePod(file, pos);
  }

  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    file.close();
    Storage.remove(filePath.c_str());
    return false;
  }

  // Write anchor-to-page map for fragment navigation (TOC + footnote targets)
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = visitor.getAnchors();
  serialization::writePod(file, static_cast<uint16_t>(anchors.size()));
  for (const auto& [anchor, page] : anchors) {
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
  }

  // Write per-page paragraph LUT: count + array of {xhtmlByteOffset(u32), paragraphIndex(u16)}.
  // The byte offset lets findXPathForParagraph seek near the target paragraph without scanning
  // from the beginning of the XHTML file, reducing SD reads on large chapters.
  const uint32_t paragraphLutOffset = file.position();
  const auto& paragraphLut = visitor.getParagraphLutPerPage();
  if (paragraphLut.size() != static_cast<size_t>(pageCount)) {
    LOG_ERR("SCT", "Paragraph LUT size mismatch: lut=%u pageCount=%u", static_cast<uint32_t>(paragraphLut.size()),
            static_cast<uint32_t>(pageCount));
    file.close();
    Storage.remove(filePath.c_str());
    return false;
  }
  serialization::writePod(file, static_cast<uint16_t>(paragraphLut.size()));
  for (const auto& entry : paragraphLut) {
    serialization::writePod(file, entry.xhtmlByteOffset);
    serialization::writePod(file, entry.paragraphIndex);
  }

  // Patch header with final pageCount, lutOffset, anchorMapOffset, and paragraphLutOffset
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 3 - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, paragraphLutOffset);
  file.close();
  if (cssParser) {
    cssParser->clear();
  }

  buildTocBoundaries(anchors);

  // Cache the LUT in memory and open the file for reading so that
  // subsequent loadPageFromSectionFile() calls can seek directly without re-opening.
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    LOG_ERR("SCT", "Failed to open section file for reading after creation");
    return false;
  }
  this->lut = std::move(lut);
  return true;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (currentPage < 0 || currentPage >= static_cast<int>(lut.size())) {
    LOG_ERR("SCT", "loadPageFromSectionFile: page %d out of LUT range (%u entries)", currentPage,
            static_cast<uint32_t>(lut.size()));
    return nullptr;
  }

  if (!file) {
    // Safety fallback: file was closed unexpectedly; reopen
    LOG_ERR("SCT", "loadPageFromSectionFile: file not open, reopening");
    if (!Storage.openFileForRead("SCT", filePath, file)) {
      return nullptr;
    }
  }

  if (!file.seek(lut[currentPage])) {
    LOG_ERR("SCT", "loadPageFromSectionFile: seek to page %d offset %u failed", currentPage, lut[currentPage]);
    return nullptr;
  }
  return Page::deserialize(file);
  // File is intentionally NOT closed; stays open for the next page load
}

// Resolve TOC anchor-to-page mappings from the parser's in-memory anchor vector.
// Called after createSectionFile when anchors are already in memory.
// See buildTocBoundariesFromFile for the on-disk variant; the two are kept separate
// because the anchor resolution has fundamentally different iteration patterns
// (scan in-memory vector vs. stream from file with early exit).
void Section::buildTocBoundaries(const std::vector<std::pair<std::string, uint16_t>>& anchors) {
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex < 0) return;

  // Count TOC entries for this spine and how many have anchors to resolve
  const int tocCount = epub->getTocItemsCount();
  uint16_t totalEntries = 0;
  uint16_t unresolvedCount = 0;
  for (int i = startTocIndex; i < tocCount; i++) {
    const auto entry = epub->getTocItem(i);
    if (entry.spineIndex != spineIndex) break;
    totalEntries++;
    if (!entry.anchor.empty()) unresolvedCount++;
  }

  // If no TOC entries have anchors, all chapters start at page 0 and
  // getTocIndexForPage falls back to epub->getTocIndexForSpineIndex,
  // so there's nothing to resolve and no value in storing boundaries.
  if (totalEntries == 0 || unresolvedCount == 0) return;

  tocBoundaries.reserve(totalEntries);
  for (int i = startTocIndex; i < startTocIndex + totalEntries; i++) {
    const auto entry = epub->getTocItem(i);
    uint16_t page = 0;
    if (!entry.anchor.empty()) {
      for (const auto& [key, val] : anchors) {
        if (key == entry.anchor) {
          page = val;
          break;
        }
      }
    }
    tocBoundaries.push_back({i, page});
  }

  // Defensive sort in case TOC entries are out of document order in a malformed epub
  std::sort(tocBoundaries.begin(), tocBoundaries.end(),
            [](const TocBoundary& a, const TocBoundary& b) { return a.startPage < b.startPage; });
}

// Resolve TOC anchor-to-page mappings by scanning the section cache's on-disk anchor data.
// Called from loadSectionFile when anchors are not in memory. Caches the small set of
// TOC anchor strings first (since getTocItem does file I/O to BookMetadataCache), then
// streams through on-disk anchors matching only those, stopping as soon as all are found.
// See buildTocBoundaries for the in-memory variant.
void Section::buildTocBoundariesFromFile(FsFile& f) {
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex < 0) return;

  // Count TOC entries for this spine, then reserve and populate
  const int tocCount = epub->getTocItemsCount();
  uint16_t totalEntries = 0;
  uint16_t unresolvedCount = 0;
  for (int i = startTocIndex; i < tocCount; i++) {
    const auto entry = epub->getTocItem(i);
    if (entry.spineIndex != spineIndex) break;
    totalEntries++;
    if (!entry.anchor.empty()) unresolvedCount++;
  }

  // If no TOC entries have anchors, all chapters start at page 0 and
  // getTocIndexForPage falls back to epub->getTocIndexForSpineIndex,
  // so there's nothing to resolve and no value in storing boundaries.
  if (totalEntries == 0 || unresolvedCount == 0) return;

  // Cache TOC anchor strings before scanning disk, since getTocItem() does file I/O
  struct TocAnchorEntry {
    int tocIndex;
    std::string anchor;
  };
  std::vector<TocAnchorEntry> tocAnchorsToResolve;
  tocAnchorsToResolve.reserve(unresolvedCount);
  tocBoundaries.reserve(totalEntries);
  for (int i = startTocIndex; i < startTocIndex + totalEntries; i++) {
    const auto entry = epub->getTocItem(i);
    tocBoundaries.push_back({i, 0});
    if (!entry.anchor.empty()) {
      tocAnchorsToResolve.push_back({i, std::move(entry.anchor)});
    }
  }

  // Single pass through on-disk anchors, matching against cached TOC anchors.
  // Stop early once all TOC anchors are resolved.
  // Header layout: ... | lutOffset (u32) | anchorMapOffset (u32) | paragraphLutOffset (u32) |
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);

  if (anchorMapOffset != 0) {
    f.seek(anchorMapOffset);
    uint16_t count;
    serialization::readPod(f, count);
    std::string key;
    for (uint16_t i = 0; i < count && unresolvedCount > 0; i++) {
      uint16_t page;
      serialization::readString(f, key);
      serialization::readPod(f, page);
      for (auto& tocAnchor : tocAnchorsToResolve) {
        if (!tocAnchor.anchor.empty() && key == tocAnchor.anchor) {
          tocBoundaries[tocAnchor.tocIndex - startTocIndex].startPage = page;
          tocAnchor.anchor.clear();  // mark resolved
          unresolvedCount--;
          break;
        }
      }
    }
  }

  // Defensive sort in case TOC entries are out of document order in a malformed epub
  std::sort(tocBoundaries.begin(), tocBoundaries.end(),
            [](const TocBoundary& a, const TocBoundary& b) { return a.startPage < b.startPage; });
}

int Section::getTocIndexForPage(const int page) const {
  if (tocBoundaries.empty()) {
    return epub->getTocIndexForSpineIndex(spineIndex);
  }

  // Find the first boundary AFTER page, then step back one
  auto it = std::upper_bound(tocBoundaries.begin(), tocBoundaries.end(), static_cast<uint16_t>(page),
                             [](uint16_t page, const TocBoundary& boundary) { return page < boundary.startPage; });
  if (it == tocBoundaries.begin()) {
    return tocBoundaries[0].tocIndex;
  }
  return std::prev(it)->tocIndex;
}

std::optional<int> Section::getPageForTocIndex(const int tocIndex) const {
  for (const auto& boundary : tocBoundaries) {
    if (boundary.tocIndex == tocIndex) {
      return boundary.startPage;
    }
  }
  return std::nullopt;
}

std::optional<Section::TocPageRange> Section::getPageRangeForTocIndex(const int tocIndex) const {
  for (size_t i = 0; i < tocBoundaries.size(); i++) {
    if (tocBoundaries[i].tocIndex == tocIndex) {
      const int startPage = tocBoundaries[i].startPage;
      const int endPage = (i + 1 < tocBoundaries.size()) ? static_cast<int>(tocBoundaries[i + 1].startPage) : pageCount;
      return TocPageRange{startPage, endPage};
    }
  }
  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    f.close();
    return std::nullopt;
  }

  f.seek(anchorMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    serialization::readString(f, key);
    serialization::readPod(f, page);
    if (key == anchor) {
      f.close();
      return page;
    }
  }

  f.close();
  return std::nullopt;
}

bool Section::readParagraphLutHeader(FsFile& outFile, uint16_t& outCount, uint32_t& outLutStart) const {
  if (!Storage.openFileForRead("SCT", filePath, outFile)) {
    return false;
  }

  const uint32_t fileSize = outFile.size();

  outFile.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t paragraphLutOffset;
  serialization::readPod(outFile, paragraphLutOffset);
  if (fileSize < sizeof(uint16_t) || paragraphLutOffset == 0 || paragraphLutOffset > fileSize - sizeof(uint16_t)) {
    outFile.close();
    return false;
  }

  outFile.seek(paragraphLutOffset);
  serialization::readPod(outFile, outCount);
  if (outCount == 0) {
    outFile.close();
    return false;
  }

  const uint64_t remainingBytes = static_cast<uint64_t>(fileSize) - paragraphLutOffset;
  const uint64_t requiredBytes = sizeof(uint16_t) + static_cast<uint64_t>(outCount) * PARAGRAPH_LUT_ENTRY_SIZE;
  if (remainingBytes < requiredBytes) {
    outFile.close();
    return false;
  }

  outLutStart = paragraphLutOffset + sizeof(uint16_t);

  return true;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  FsFile f;
  uint16_t count = 0;
  uint32_t lutStart = 0;
  if (!readParagraphLutHeader(f, count, lutStart)) {
    return std::nullopt;
  }
  const uint32_t fileSize = f.size();

  // Each LUT entry stores the paragraph index at page-break time — i.e. the last
  // <p> whose start tag had been seen while page i was being laid out. Paragraph
  // P therefore first appears on the smallest i where storedPIdx[i] >= P.
  for (uint16_t i = 0; i < count; i++) {
    const uint32_t entryOffset = paragraphLutEntryOffset(lutStart, i) + sizeof(uint32_t);
    const uint64_t requiredOffset = static_cast<uint64_t>(entryOffset) + sizeof(uint16_t);
    if (requiredOffset > fileSize) {
      f.close();
      return std::nullopt;
    }
    f.seek(entryOffset);
    uint16_t pagePIdx;
    serialization::readPod(f, pagePIdx);
    if (pagePIdx >= pIndex) {
      f.close();
      return i;
    }
  }

  f.close();
  return static_cast<uint16_t>(count - 1);
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  FsFile f;
  uint16_t count = 0;
  uint32_t lutStart = 0;
  if (!readParagraphLutHeader(f, count, lutStart)) {
    return std::nullopt;
  }
  if (page >= count) {
    f.close();
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  const uint32_t entryOffset = paragraphLutEntryOffset(lutStart, page) + sizeof(uint32_t);
  const uint64_t requiredOffset = static_cast<uint64_t>(entryOffset) + sizeof(uint16_t);
  if (requiredOffset > fileSize) {
    f.close();
    return std::nullopt;
  }

  // Seek directly to the paragraphIndex field of the requested entry (skip xhtmlByteOffset)
  f.seek(entryOffset);
  uint16_t pIdx;
  serialization::readPod(f, pIdx);

  f.close();
  return pIdx;
}

std::optional<uint32_t> Section::getXhtmlByteOffsetForPage(const uint16_t page) const {
  FsFile f;
  uint16_t count = 0;
  uint32_t lutStart = 0;
  if (!readParagraphLutHeader(f, count, lutStart)) {
    return std::nullopt;
  }
  if (page >= count) {
    f.close();
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  const uint32_t entryOffset = paragraphLutEntryOffset(lutStart, page);
  const uint64_t requiredOffset = static_cast<uint64_t>(entryOffset) + sizeof(uint32_t);
  if (requiredOffset > fileSize) {
    f.close();
    return std::nullopt;
  }

  f.seek(entryOffset);
  uint32_t byteOffset;
  serialization::readPod(f, byteOffset);

  f.close();
  // A zero offset means the entry was recorded post-parse (last page), so it's unusable as a hint.
  return byteOffset > 0 ? std::optional<uint32_t>{byteOffset} : std::nullopt;
}
