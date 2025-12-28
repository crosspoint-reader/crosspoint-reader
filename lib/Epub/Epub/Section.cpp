#include "Section.h"

#include <FsHelpers.h>
#include <SD.h>
#include <Serialization.h>

#include "Page.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint8_t SECTION_FILE_VERSION = 6;
}  // namespace

void Section::onPageComplete(std::unique_ptr<Page> page) {
  const auto filePath = cachePath + "/page_" + std::to_string(pageCount) + ".bin";

  File outputFile;
  if (!FsHelpers::openFileForWrite("SCT", filePath, outputFile)) {
    return;
  }
  page->serialize(outputFile);
  outputFile.close();

  Serial.printf("[%lu] [SCT] Page %d processed\n", millis(), pageCount);

  pageCount++;
}

void Section::writeCacheMetadata(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                 const int viewportWidth, const int viewportHeight) const {
  File outputFile;
  if (!FsHelpers::openFileForWrite("SCT", cachePath + "/section.bin", outputFile)) {
    return;
  }
  serialization::writePod(outputFile, SECTION_FILE_VERSION);
  serialization::writePod(outputFile, fontId);
  serialization::writePod(outputFile, lineCompression);
  serialization::writePod(outputFile, extraParagraphSpacing);
  serialization::writePod(outputFile, viewportWidth);
  serialization::writePod(outputFile, viewportHeight);
  serialization::writePod(outputFile, pageCount);
  outputFile.close();
}

bool Section::loadCacheMetadata(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const int viewportWidth, const int viewportHeight) {
  const auto sectionFilePath = cachePath + "/section.bin";
  File inputFile;
  if (!FsHelpers::openFileForRead("SCT", sectionFilePath, inputFile)) {
    return false;
  }

  // Match parameters
  {
    uint8_t version;
    serialization::readPod(inputFile, version);
    if (version != SECTION_FILE_VERSION) {
      inputFile.close();
      Serial.printf("[%lu] [SCT] Deserialization failed: Unknown version %u\n", millis(), version);
      clearCache();
      return false;
    }

    int fileFontId, fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    serialization::readPod(inputFile, fileFontId);
    serialization::readPod(inputFile, fileLineCompression);
    serialization::readPod(inputFile, fileExtraParagraphSpacing);
    serialization::readPod(inputFile, fileViewportWidth);
    serialization::readPod(inputFile, fileViewportHeight);

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || viewportWidth != fileViewportWidth ||
        viewportHeight != fileViewportHeight) {
      inputFile.close();
      Serial.printf("[%lu] [SCT] Deserialization failed: Parameters do not match\n", millis());
      clearCache();
      return false;
    }
  }

  serialization::readPod(inputFile, pageCount);
  inputFile.close();
  Serial.printf("[%lu] [SCT] Deserialization succeeded: %d pages\n", millis(), pageCount);
  return true;
}

void Section::setupCacheDir() const {
  epub->setupCacheDir();
  SD.mkdir(cachePath.c_str());
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  if (!SD.exists(cachePath.c_str())) {
    Serial.printf("[%lu] [SCT] Cache does not exist, no action needed\n", millis());
    return true;
  }

  if (!FsHelpers::removeDir(cachePath.c_str())) {
    Serial.printf("[%lu] [SCT] Failed to clear cache\n", millis());
    return false;
  }

  Serial.printf("[%lu] [SCT] Cache cleared successfully\n", millis());
  return true;
}

bool Section::persistPageDataToSD(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                  const int viewportWidth, const int viewportHeight,
                                  const std::function<void()>& progressSetupFn,
                                  const std::function<void(int)>& progressFn) {
  constexpr size_t MIN_SIZE_FOR_PROGRESS = 50 * 1024;  // 50KB
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Retry logic for SD card timing issues
  bool success = false;
  size_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      Serial.printf("[%lu] [SCT] Retrying stream (attempt %d)...\n", millis(), attempt + 1);
      delay(50);  // Brief delay before retry
    }

    // Remove any incomplete file from previous attempt before retrying
    if (SD.exists(tmpHtmlPath.c_str())) {
      SD.remove(tmpHtmlPath.c_str());
    }

    File tmpHtml;
    if (!FsHelpers::openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    tmpHtml.close();

    // If streaming failed, remove the incomplete file immediately
    if (!success && SD.exists(tmpHtmlPath.c_str())) {
      SD.remove(tmpHtmlPath.c_str());
      Serial.printf("[%lu] [SCT] Removed incomplete temp file after failed attempt\n", millis());
    }
  }

  if (!success) {
    Serial.printf("[%lu] [SCT] Failed to stream item contents to temp file after retries\n", millis());
    return false;
  }

  Serial.printf("[%lu] [SCT] Streamed temp HTML to %s (%d bytes)\n", millis(), tmpHtmlPath.c_str(), fileSize);

  // Only show progress bar for larger chapters where rendering overhead is worth it
  if (progressSetupFn && fileSize >= MIN_SIZE_FOR_PROGRESS) {
    progressSetupFn();
  }

  ChapterHtmlSlimParser visitor(
      tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, viewportWidth, viewportHeight,
      [this](std::unique_ptr<Page> page) { this->onPageComplete(std::move(page)); }, progressFn);
  success = visitor.parseAndBuildPages();

  SD.remove(tmpHtmlPath.c_str());
  if (!success) {
    Serial.printf("[%lu] [SCT] Failed to parse XML and build pages\n", millis());
    return false;
  }

  writeCacheMetadata(fontId, lineCompression, extraParagraphSpacing, viewportWidth, viewportHeight);

  return true;
}

std::unique_ptr<Page> Section::loadPageFromSD() const {
  const auto filePath = cachePath + "/page_" + std::to_string(currentPage) + ".bin";

  File inputFile;
  if (!FsHelpers::openFileForRead("SCT", filePath, inputFile)) {
    return nullptr;
  }
  auto page = Page::deserialize(inputFile);
  inputFile.close();
  return page;
}
