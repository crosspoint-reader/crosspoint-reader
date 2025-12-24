#include "Section.h"

#include <FsHelpers.h>
#include <SD.h>
#include <Serialization.h>

#include "Page.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint8_t SECTION_FILE_VERSION = 5;
}

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

void Section::writeCacheMetadata(const int fontId, const float lineCompression, const int marginTop,
                                 const int marginRight, const int marginBottom, const int marginLeft,
                                 const bool extraParagraphSpacing) const {
  File outputFile;
  if (!FsHelpers::openFileForWrite("SCT", cachePath + "/section.bin", outputFile)) {
    return;
  }
  serialization::writePod(outputFile, SECTION_FILE_VERSION);
  serialization::writePod(outputFile, fontId);
  serialization::writePod(outputFile, lineCompression);
  serialization::writePod(outputFile, marginTop);
  serialization::writePod(outputFile, marginRight);
  serialization::writePod(outputFile, marginBottom);
  serialization::writePod(outputFile, marginLeft);
  serialization::writePod(outputFile, extraParagraphSpacing);
  serialization::writePod(outputFile, pageCount);
  outputFile.close();
}

bool Section::loadCacheMetadata(const int fontId, const float lineCompression, const int marginTop,
                                const int marginRight, const int marginBottom, const int marginLeft,
                                const bool extraParagraphSpacing) {
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

    int fileFontId, fileMarginTop, fileMarginRight, fileMarginBottom, fileMarginLeft;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    serialization::readPod(inputFile, fileFontId);
    serialization::readPod(inputFile, fileLineCompression);
    serialization::readPod(inputFile, fileMarginTop);
    serialization::readPod(inputFile, fileMarginRight);
    serialization::readPod(inputFile, fileMarginBottom);
    serialization::readPod(inputFile, fileMarginLeft);
    serialization::readPod(inputFile, fileExtraParagraphSpacing);

    if (fontId != fileFontId || lineCompression != fileLineCompression || marginTop != fileMarginTop ||
        marginRight != fileMarginRight || marginBottom != fileMarginBottom || marginLeft != fileMarginLeft ||
        extraParagraphSpacing != fileExtraParagraphSpacing) {
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

bool Section::persistPageDataToSD(const int fontId, const float lineCompression, const int marginTop,
                                  const int marginRight, const int marginBottom, const int marginLeft,
                                  const bool extraParagraphSpacing) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";
  File tmpHtml;
  if (!FsHelpers::openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
    return false;
  }
  bool success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
  tmpHtml.close();

  if (!success) {
    Serial.printf("[%lu] [SCT] Failed to stream item contents to temp file\n", millis());
    return false;
  }

  Serial.printf("[%lu] [SCT] Streamed temp HTML to %s\n", millis(), tmpHtmlPath.c_str());

  ChapterHtmlSlimParser visitor(tmpHtmlPath, renderer, fontId, lineCompression, marginTop, marginRight, marginBottom,
                                marginLeft, extraParagraphSpacing,
                                [this](std::unique_ptr<Page> page) { this->onPageComplete(std::move(page)); });
  success = visitor.parseAndBuildPages();

  SD.remove(tmpHtmlPath.c_str());
  if (!success) {
    Serial.printf("[%lu] [SCT] Failed to parse XML and build pages\n", millis());
    return false;
  }

  writeCacheMetadata(fontId, lineCompression, marginTop, marginRight, marginBottom, marginLeft, extraParagraphSpacing);

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
