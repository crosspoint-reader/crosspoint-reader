#include "Section.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint8_t SECTION_FILE_VERSION = 16;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint32_t);
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
                                     const bool embeddedStyle) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(embeddedStyle) + sizeof(uint32_t),
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
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0 when written)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle) {
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
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileEmbeddedStyle);

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
        viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
        hyphenationEnabled != fileHyphenationEnabled || embeddedStyle != fileEmbeddedStyle) {
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
                         viewportHeight, hyphenationEnabled, embeddedStyle);
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

  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
      viewportHeight, hyphenationEnabled,
      [this, &lut](std::unique_ptr<Page> page) { lut.emplace_back(this->onPageComplete(std::move(page))); },
      embeddedStyle, contentBase, imageBasePath, popupFn, cssParser);
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

  // Go back and write LUT offset
  file.seek(HEADER_SIZE - sizeof(uint32_t) - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  file.close();
  if (cssParser) {
    cssParser->clear();
  }

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
