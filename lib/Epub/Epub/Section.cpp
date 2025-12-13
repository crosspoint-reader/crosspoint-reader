#include "Section.h"

#include <SD.h>
#include <Serialization.h>

#include <fstream>

#include "FsHelpers.h"
#include "Page.h"
#include "parsers/ChapterHtmlSlimParser.h"

constexpr uint8_t SECTION_FILE_VERSION = 4;

void Section::onPageComplete(std::unique_ptr<Page> page) {
  const auto filePath = cachePath + "/page_" + std::to_string(pageCount) + ".bin";

  std::ofstream outputFile("/sd" + filePath);
  page->serialize(outputFile);
  outputFile.close();

  Serial.printf("[%lu] [SCT] Page %d processed\n", millis(), pageCount);

  pageCount++;
}

void Section::writeCacheMetadata(const int fontId, const float lineCompression, const int marginTop,
                                 const int marginRight, const int marginBottom, const int marginLeft) const {
  std::ofstream outputFile(("/sd" + cachePath + "/section.bin").c_str());
  serialization::writePod(outputFile, SECTION_FILE_VERSION);
  serialization::writePod(outputFile, fontId);
  serialization::writePod(outputFile, lineCompression);
  serialization::writePod(outputFile, marginTop);
  serialization::writePod(outputFile, marginRight);
  serialization::writePod(outputFile, marginBottom);
  serialization::writePod(outputFile, marginLeft);
  serialization::writePod(outputFile, pageCount);
  outputFile.close();
}

bool Section::loadCacheMetadata(const int fontId, const float lineCompression, const int marginTop,
                                const int marginRight, const int marginBottom, const int marginLeft) {
  if (!SD.exists(cachePath.c_str())) {
    return false;
  }

  const auto sectionFilePath = cachePath + "/section.bin";
  if (!SD.exists(sectionFilePath.c_str())) {
    return false;
  }

  std::ifstream inputFile(("/sd" + sectionFilePath).c_str());

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
    serialization::readPod(inputFile, fileFontId);
    serialization::readPod(inputFile, fileLineCompression);
    serialization::readPod(inputFile, fileMarginTop);
    serialization::readPod(inputFile, fileMarginRight);
    serialization::readPod(inputFile, fileMarginBottom);
    serialization::readPod(inputFile, fileMarginLeft);

    if (fontId != fileFontId || lineCompression != fileLineCompression || marginTop != fileMarginTop ||
        marginRight != fileMarginRight || marginBottom != fileMarginBottom || marginLeft != fileMarginLeft) {
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
                                  const int marginRight, const int marginBottom, const int marginLeft) {
  const auto localPath = epub->getSpineItem(spineIndex);

  // TODO: Should we get rid of this file all together?
  //       It currently saves us a bit of memory by allowing for all the inflation bits to be released
  //       before loading the XML parser
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";
  File f = SD.open(tmpHtmlPath.c_str(), FILE_WRITE, true);
  bool success = epub->readItemContentsToStream(localPath, f, 1024);
  f.close();

  if (!success) {
    Serial.printf("[%lu] [SCT] Failed to stream item contents to temp file\n", millis());
    return false;
  }

  Serial.printf("[%lu] [SCT] Streamed temp HTML to %s\n", millis(), tmpHtmlPath.c_str());

  const auto sdTmpHtmlPath = "/sd" + tmpHtmlPath;

  ChapterHtmlSlimParser visitor(sdTmpHtmlPath.c_str(), renderer, fontId, lineCompression, marginTop, marginRight,
                                marginBottom, marginLeft,
                                [this](std::unique_ptr<Page> page) { this->onPageComplete(std::move(page)); });
  success = visitor.parseAndBuildPages();

  SD.remove(tmpHtmlPath.c_str());
  if (!success) {
    Serial.printf("[%lu] [SCT] Failed to parse XML and build pages\n", millis());
    return false;
  }

  writeCacheMetadata(fontId, lineCompression, marginTop, marginRight, marginBottom, marginLeft);

  return true;
}

std::unique_ptr<Page> Section::loadPageFromSD() const {
  const auto filePath = "/sd" + cachePath + "/page_" + std::to_string(currentPage) + ".bin";
  if (!SD.exists(filePath.c_str() + 3)) {
    Serial.printf("[%lu] [SCT] Page file does not exist: %s\n", millis(), filePath.c_str());
    return nullptr;
  }

  std::ifstream inputFile(filePath);
  auto page = Page::deserialize(inputFile);
  inputFile.close();
  return page;
}
