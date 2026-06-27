#include "Section.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>

#include "AsciiCase.h"
#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
// v28: page LUT entries include offsets to compact text records used by search.
constexpr uint8_t SECTION_FILE_VERSION = 28;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint8_t) + sizeof(bool) + sizeof(uint32_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t);

struct PageLutEntry {
  uint32_t fileOffset;
  uint32_t searchTextOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
};
static_assert(sizeof(PageLutEntry) == 12, "Unexpected PageLutEntry padding changes the transient RAM budget");

// On-disk page LUT stride: only pageOffset and searchTextOffset are stored
// inline; paragraphIndex and listItemIndex are written to separate LUTs.
constexpr size_t PAGE_LUT_ENTRY_SIZE = sizeof(uint32_t) * 2;
// Bind the stride to the two inline offset fields so adding or resizing an
// inline LUT field can't silently desync it from the write/read sites.
static_assert(PAGE_LUT_ENTRY_SIZE == sizeof(PageLutEntry::fileOffset) + sizeof(PageLutEntry::searchTextOffset),
              "On-disk page-LUT stride must match the inline offset fields");
}  // namespace

uint32_t Section::onPageComplete(std::unique_ptr<Page> page, uint32_t& searchTextOffset) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  searchTextOffset = file.position();
  if (!page->serializeSearchText(file)) {
    LOG_ERR("SCT", "Failed to serialize search text for page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", pageCount);

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                     const uint16_t viewportHeight, const bool hyphenationEnabled,
                                     const bool embeddedStyle, const uint8_t imageRendering,
                                     const bool focusReadingEnabled) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(embeddedStyle) + sizeof(imageRendering) + sizeof(focusReadingEnabled) +
                                   sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t),
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
  serialization::writePod(file, focusReadingEnabled);
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for paragraph LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for li LUT offset (patched later)
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                              const uint8_t imageRendering, const bool focusReadingEnabled) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  {
    uint8_t version;
    serialization::readPod(file, version);
    if (version != SECTION_FILE_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
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
    bool fileFocusReadingEnabled;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileEmbeddedStyle);
    serialization::readPod(file, fileImageRendering);
    serialization::readPod(file, fileFocusReadingEnabled);

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
        viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
        hyphenationEnabled != fileHyphenationEnabled || embeddedStyle != fileEmbeddedStyle ||
        imageRendering != fileImageRendering || focusReadingEnabled != fileFocusReadingEnabled) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  serialization::readPod(file, pageCount);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages", pageCount);
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
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
                                const uint8_t imageRendering, const bool focusReadingEnabled,
                                const std::function<void()>& popupFn) {
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

    HalFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    // Explicitly close() file before calling Storage.remove()
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
                         viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled);
  std::vector<PageLutEntry> lut = {};
  // Reserve 128 * sizeof(PageLutEntry) = 1,536 bytes once on the heap. The
  // count is data-dependent, so stack/static storage would impose a hard page
  // cap; reserving avoids the common alloc-copy-free growth cycle.
  lut.reserve(128);

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
      viewportHeight, hyphenationEnabled, focusReadingEnabled,
      [this, &lut](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex) {
        uint32_t searchTextOffset = 0;
        const uint32_t fileOffset = this->onPageComplete(std::move(page), searchTextOffset);
        lut.push_back({fileOffset, searchTextOffset, paragraphIndex, listItemIndex});
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, std::move(tocAnchors), popupFn, cssParser);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  success = visitor.parseAndBuildPages();

  Storage.remove(tmpHtmlPath.c_str());
  if (!success) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    // Explicitly close() file before calling Storage.remove()
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
  for (const auto& entry : lut) {
    if (entry.fileOffset == 0 || entry.searchTextOffset == 0) {
      hasFailedLutRecords = true;
      break;
    }
    serialization::writePod(file, entry.fileOffset);
    serialization::writePod(file, entry.searchTextOffset);
  }

  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filePath.c_str());
    return false;
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets)
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = visitor.getAnchors();
  serialization::writePod(file, static_cast<uint16_t>(anchors.size()));
  for (const auto& [anchor, page] : anchors) {
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
  }

  const uint32_t paragraphLutOffset = file.position();
  serialization::writePod(file, static_cast<uint16_t>(lut.size()));
  for (const auto& entry : lut) {
    serialization::writePod(file, entry.paragraphIndex);
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : lut) {
    serialization::writePod(file, entry.listItemIndex);
  }

  // Patch header with final pageCount, lutOffset, anchorMapOffset, paragraphLutOffset, and liLutOffset
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, paragraphLutOffset);
  serialization::writePod(file, liLutFileOffset);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  if (cssParser) {
    cssParser->clear();
  }
  return true;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return nullptr;
  }

  file.seek(HEADER_SIZE - sizeof(uint32_t) * 4);
  uint32_t lutOffset;
  serialization::readPod(file, lutOffset);
  file.seek(lutOffset + PAGE_LUT_ENTRY_SIZE * currentPage);
  uint32_t pagePos;
  serialization::readPod(file, pagePos);
  file.seek(pagePos);

  auto page = Page::deserialize(file);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  return page;
}

void Section::rebuildFilePathForSpine() {
  // Re-append the "<spineIndex>.bin" suffix onto the cached prefix in place.
  // snprintf into a stack buffer avoids std::to_string's heap temporary, and
  // resize()+append() reuse filePath's reserved capacity (no reallocation).
  char numBuf[12];
  const int len = snprintf(numBuf, sizeof(numBuf), "%d", spineIndex);
  filePath.resize(sectionPathPrefixLen);
  if (len > 0) {
    filePath.append(numBuf, static_cast<size_t>(len));
  }
  filePath.append(".bin");
}

void Section::resetForSpine(const int newSpineIndex) {
  if (file) {
    // Member handle persists across calls, so close before switching paths.
    file.close();
  }
  spineIndex = newSpineIndex;
  rebuildFilePathForSpine();
  pageCount = 0;
  currentPage = 0;
  searchHeaderReady = false;
}

bool Section::ensureSearchHeader() {
  if (searchHeaderReady) {
    return true;
  }

  // Open the member file handle lazily on the first call. It stays open for
  // all pages in this section; resetForSpine() closes it when advancing.
  if (!file) {
    if (!Storage.openFileForRead("SCT", filePath, file)) {
      return false;
    }
  }

  const uint32_t fileSize = file.size();
  if (fileSize < HEADER_SIZE) {
    LOG_ERR("SCT", "Search failed: section cache header is truncated");
    return false;
  }

  file.seek(HEADER_SIZE - sizeof(uint32_t) * 4);
  uint32_t lutOffset = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&lutOffset), sizeof(lutOffset)) != sizeof(lutOffset)) {
    LOG_ERR("SCT", "Search failed: could not read page LUT offset");
    return false;
  }

  searchFileSize = fileSize;
  searchLutOffset = lutOffset;
  searchHeaderReady = true;
  return true;
}

bool Section::isValidSearchQuery(const std::string_view query) {
  if (query.empty() || query.size() > MAX_SEARCH_QUERY_BYTES) {
    return false;
  }
  // Reject all-whitespace queries: at least one non-whitespace byte is required.
  return std::any_of(query.begin(), query.end(), [](const unsigned char value) { return std::isspace(value) == 0; });
}

bool Section::buildSearchPrefix(const std::string_view query, std::array<uint8_t, MAX_SEARCH_QUERY_BYTES>& prefix) {
  // Always leave the table in a defined (zeroed) state, even on rejection, so a
  // caller that ignores the return value never reads stale prefix bytes.
  prefix.fill(0);
  if (query.empty() || query.size() > MAX_SEARCH_QUERY_BYTES) {
    return false;
  }

  for (size_t i = 1, matched = 0; i < query.size(); ++i) {
    const uint8_t value = epub::asciiToLower(static_cast<uint8_t>(query[i]));
    while (matched > 0 && value != epub::asciiToLower(static_cast<uint8_t>(query[matched]))) {
      matched = prefix[matched - 1];
    }
    if (value == epub::asciiToLower(static_cast<uint8_t>(query[matched]))) {
      ++matched;
    }
    prefix[i] = static_cast<uint8_t>(matched);
  }
  return true;
}

std::optional<bool> Section::pageContainsText(const uint16_t page, const std::string_view query,
                                              const std::array<uint8_t, MAX_SEARCH_QUERY_BYTES>& prefix) {
  if (query.empty() || query.size() > MAX_SEARCH_QUERY_BYTES || page >= pageCount) {
    LOG_ERR("SCT", "Invalid page search request (page=%u count=%u queryBytes=%u)", page, pageCount,
            static_cast<unsigned>(query.size()));
    return std::nullopt;
  }

  // File size and page-LUT offset are invariant per section; read them once.
  if (!ensureSearchHeader()) {
    return std::nullopt;
  }
  const uint32_t fileSize = searchFileSize;
  const uint32_t lutOffset = searchLutOffset;

  // Compute in 64-bit so a corrupt (huge) lutOffset cannot wrap the uint32 sum
  // into a small in-bounds value that slips past the fileSize bounds check.
  const uint64_t entryOffset = static_cast<uint64_t>(lutOffset) + static_cast<uint64_t>(PAGE_LUT_ENTRY_SIZE) * page;
  if (lutOffset == 0 || entryOffset > fileSize || fileSize - entryOffset < PAGE_LUT_ENTRY_SIZE) {
    LOG_ERR("SCT", "Search failed: invalid page LUT entry");
    return std::nullopt;
  }

  file.seek(static_cast<size_t>(entryOffset) + sizeof(uint32_t));
  uint32_t searchTextOffset = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&searchTextOffset), sizeof(searchTextOffset)) != sizeof(searchTextOffset) ||
      searchTextOffset > fileSize || fileSize - searchTextOffset < sizeof(uint32_t)) {
    LOG_ERR("SCT", "Search failed: invalid text record offset");
    return std::nullopt;
  }

  file.seek(searchTextOffset);
  uint32_t remaining = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&remaining), sizeof(remaining)) != sizeof(remaining) ||
      remaining > fileSize - searchTextOffset - sizeof(uint32_t)) {
    LOG_ERR("SCT", "Search failed: invalid text record length");
    return std::nullopt;
  }

  // KMP keeps overlap handling correct while streaming through a 64-byte SD
  // read buffer. The prefix table is built once per search by the caller
  // (buildSearchPrefix) and reused across every page rather than rebuilt here.
  std::array<uint8_t, 64> buffer{};
  size_t matched = 0;
  while (remaining > 0) {
    const size_t chunkSize = std::min<size_t>(buffer.size(), remaining);
    if (file.read(buffer.data(), chunkSize) != chunkSize) {
      LOG_ERR("SCT", "Search failed: truncated text record");
      return std::nullopt;
    }
    remaining -= chunkSize;

    for (size_t i = 0; i < chunkSize; ++i) {
      const uint8_t value = epub::asciiToLower(buffer[i]);
      while (matched > 0 && value != epub::asciiToLower(static_cast<uint8_t>(query[matched]))) {
        matched = prefix[matched - 1];
      }
      if (value == epub::asciiToLower(static_cast<uint8_t>(query[matched]))) {
        ++matched;
        if (matched == query.size()) {
          return true;
        }
      }
    }
  }

  return false;
}

std::string Section::getTextFromSectionFile() {
  std::string fullText;
  auto p = this->loadPageFromSectionFile();
  if (p) {
    for (const auto& el : p->elements) {
      if (el->getTag() == TAG_PageLine) {
        const auto& line = static_cast<const PageLine&>(*el);
        if (line.getBlock()) {
          const auto& words = line.getBlock()->getWords();
          for (const auto& w : words) {
            if (!fullText.empty()) fullText += " ";
            fullText += w;
          }
        }
      }
    }
  }
  return fullText;
}

std::optional<uint16_t> Section::getCachedPageCount() const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE) {
    return std::nullopt;
  }

  f.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(uint16_t));
  uint16_t count;
  serialization::readPod(f, count);
  return count;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 3);
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
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
      return page;
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = paragraphLutOffset + sizeof(uint16_t) + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx;
    serialization::readPod(f, pagePIdx);
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset + sizeof(uint16_t) + page * sizeof(uint16_t));
  uint16_t pIdx;
  serialization::readPod(f, pIdx);
  return pIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t liLutOffset;
  serialization::readPod(f, liLutOffset);
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = liLutOffset + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(liLutOffset);
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx;
    serialization::readPod(f, pageLiIdx);
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}
