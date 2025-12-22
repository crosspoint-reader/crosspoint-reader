#include "Epub.h"

#include <HardwareSerial.h>
#include <JpegToBmpConverter.h>
#include <SD.h>
#include <ZipFile.h>

#include <map>

#include "Epub/FsHelpers.h"
#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/TocNcxParser.h"

bool Epub::findContentOpfFile(std::string* contentOpfFile) const {
  const auto containerPath = "META-INF/container.xml";
  size_t containerSize;

  // Get file size without loading it all into heap
  if (!getItemSize(containerPath, &containerSize)) {
    Serial.printf("[%lu] [EBP] Could not find or size META-INF/container.xml\n", millis());
    return false;
  }

  ContainerParser containerParser(containerSize);

  if (!containerParser.setup()) {
    return false;
  }

  // Stream read (reusing your existing stream logic)
  if (!readItemContentsToStream(containerPath, containerParser, 512)) {
    Serial.printf("[%lu] [EBP] Could not read META-INF/container.xml\n", millis());
    return false;
  }

  // Extract the result
  if (containerParser.fullPath.empty()) {
    Serial.printf("[%lu] [EBP] Could not find valid rootfile in container.xml\n", millis());
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);
  return true;
}

bool Epub::parseContentOpf(bool useCache) {
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    Serial.printf("[%lu] [EBP] Could not find content.opf in zip\n", millis());
    return false;
  }

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  Serial.printf("[%lu] [EBP] Parsing content.opf: %s\n", millis(), contentOpfFilePath.c_str());

  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    Serial.printf("[%lu] [EBP] Could not get size of content.opf\n", millis());
    return false;
  }

  ContentOpfParser opfParser(getBasePath(), contentOpfSize, useCache ? spineTocCache.get() : nullptr);

  if (!opfParser.setup()) {
    Serial.printf("[%lu] [EBP] Could not setup content.opf parser\n", millis());
    return false;
  }

  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024)) {
    Serial.printf("[%lu] [EBP] Could not read content.opf\n", millis());
    return false;
  }

  // Grab data from opfParser into epub
  title = opfParser.title;
  if (!opfParser.coverItemId.empty() && opfParser.items.count(opfParser.coverItemId) > 0) {
    coverImageItem = opfParser.items.at(opfParser.coverItemId);
  }

  if (!opfParser.tocNcxPath.empty()) {
    tocNcxItem = opfParser.tocNcxPath;
  }

  Serial.printf("[%lu] [EBP] Successfully parsed content.opf\n", millis());
  return true;
}

bool Epub::parseTocNcxFile() const {
  // the ncx file should have been specified in the content.opf file
  if (tocNcxItem.empty()) {
    Serial.printf("[%lu] [EBP] No ncx file specified\n", millis());
    return false;
  }

  Serial.printf("[%lu] [EBP] Parsing toc ncx file: %s\n", millis(), tocNcxItem.c_str());

  const auto tmpNcxPath = getCachePath() + "/toc.ncx";
  File tempNcxFile = SD.open(tmpNcxPath.c_str(), FILE_WRITE);
  readItemContentsToStream(tocNcxItem, tempNcxFile, 1024);
  tempNcxFile.close();
  tempNcxFile = SD.open(tmpNcxPath.c_str(), FILE_READ);
  const auto ncxSize = tempNcxFile.size();

  TocNcxParser ncxParser(contentBasePath, ncxSize, spineTocCache.get());

  if (!ncxParser.setup()) {
    Serial.printf("[%lu] [EBP] Could not setup toc ncx parser\n", millis());
    return false;
  }

  const auto ncxBuffer = static_cast<uint8_t*>(malloc(1024));
  if (!ncxBuffer) {
    Serial.printf("[%lu] [EBP] Could not allocate memory for toc ncx parser\n", millis());
    return false;
  }

  while (tempNcxFile.available()) {
    const auto readSize = tempNcxFile.read(ncxBuffer, 1024);
    const auto processedSize = ncxParser.write(ncxBuffer, readSize);

    if (processedSize != readSize) {
      Serial.printf("[%lu] [EBP] Could not process all toc ncx data\n", millis());
      free(ncxBuffer);
      tempNcxFile.close();
      return false;
    }
  }

  free(ncxBuffer);
  tempNcxFile.close();
  SD.remove(tmpNcxPath.c_str());

  Serial.printf("[%lu] [EBP] Parsed TOC items\n", millis());
  return true;
}

// load in the meta data for the epub file
bool Epub::load() {
  Serial.printf("[%lu] [EBP] Loading ePub: %s\n", millis(), filepath.c_str());

  // Initialize spine/TOC cache
  spineTocCache.reset(new SpineTocCache(cachePath));

  // Try to load existing cache first
  if (spineTocCache->load()) {
    Serial.printf("[%lu] [EBP] Loaded spine/TOC from cache\n", millis());

    // Still need to parse content.opf for title and cover
    if (!parseContentOpf(false)) {
      Serial.printf("[%lu] [EBP] Could not parse content.opf\n", millis());
      return false;
    }

    Serial.printf("[%lu] [EBP] Loaded ePub: %s\n", millis(), filepath.c_str());
    return true;
  }

  // Cache doesn't exist or is invalid, build it
  Serial.printf("[%lu] [EBP] Cache not found, building spine/TOC cache\n", millis());
  setupCacheDir();

  // Begin building cache - stream entries to disk immediately
  if (!spineTocCache->beginWrite()) {
    Serial.printf("[%lu] [EBP] Could not begin writing cache\n", millis());
    return false;
  }
  if (!parseContentOpf(true)) {
    Serial.printf("[%lu] [EBP] Could not parse content.opf\n", millis());
    return false;
  }
  if (!parseTocNcxFile()) {
    Serial.printf("[%lu] [EBP] Could not parse toc\n", millis());
    return false;
  }
  // Close the cache files
  if (!spineTocCache->endWrite()) {
    Serial.printf("[%lu] [EBP] Could not end writing cache\n", millis());
    return false;
  }

  // Now compute mappings and sizes (this loads entries temporarily, computes, then rewrites)
  if (!spineTocCache->updateMappingsAndSizes(filepath)) {
    Serial.printf("[%lu] [EBP] Could not update mappings and sizes\n", millis());
    return false;
  }

  // Reload the cache from disk so it's in the correct state
  spineTocCache.reset(new SpineTocCache(cachePath));
  if (!spineTocCache->load()) {
    Serial.printf("[%lu] [EBP] Failed to reload cache after writing\n", millis());
    return false;
  }

  Serial.printf("[%lu] [EBP] Loaded ePub: %s\n", millis(), filepath.c_str());
  return true;
}

bool Epub::clearCache() const {
  if (!SD.exists(cachePath.c_str())) {
    Serial.printf("[%lu] [EPB] Cache does not exist, no action needed\n", millis());
    return true;
  }

  if (!FsHelpers::removeDir(cachePath.c_str())) {
    Serial.printf("[%lu] [EPB] Failed to clear cache\n", millis());
    return false;
  }

  Serial.printf("[%lu] [EPB] Cache cleared successfully\n", millis());
  return true;
}

void Epub::setupCacheDir() const {
  if (SD.exists(cachePath.c_str())) {
    return;
  }

  // Loop over each segment of the cache path and create directories as needed
  for (size_t i = 1; i < cachePath.length(); i++) {
    if (cachePath[i] == '/') {
      SD.mkdir(cachePath.substr(0, i).c_str());
    }
  }
  SD.mkdir(cachePath.c_str());
}

const std::string& Epub::getCachePath() const { return cachePath; }

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const { return title; }

std::string Epub::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Epub::generateCoverBmp() const {
  // Already generated, return true
  if (SD.exists(getCoverBmpPath().c_str())) {
    return true;
  }

  if (coverImageItem.empty()) {
    Serial.printf("[%lu] [EBP] No known cover image\n", millis());
    return false;
  }

  if (coverImageItem.substr(coverImageItem.length() - 4) == ".jpg" ||
      coverImageItem.substr(coverImageItem.length() - 5) == ".jpeg") {
    Serial.printf("[%lu] [EBP] Generating BMP from JPG cover image\n", millis());
    File coverJpg = SD.open((getCachePath() + "/.cover.jpg").c_str(), FILE_WRITE, true);
    readItemContentsToStream(coverImageItem, coverJpg, 1024);
    coverJpg.close();

    coverJpg = SD.open((getCachePath() + "/.cover.jpg").c_str(), FILE_READ);
    File coverBmp = SD.open(getCoverBmpPath().c_str(), FILE_WRITE, true);
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp);
    coverJpg.close();
    coverBmp.close();
    SD.remove((getCachePath() + "/.cover.jpg").c_str());

    if (!success) {
      Serial.printf("[%lu] [EBP] Failed to generate BMP from JPG cover image\n", millis());
      SD.remove(getCoverBmpPath().c_str());
    }
    Serial.printf("[%lu] [EBP] Generated BMP from JPG cover image, success: %s\n", millis(), success ? "yes" : "no");
    return success;
  } else {
    Serial.printf("[%lu] [EBP] Cover image is not a JPG, skipping\n", millis());
  }

  return false;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, const bool trailingNullByte) const {
  const ZipFile zip("/sd" + filepath);
  const std::string path = FsHelpers::normalisePath(itemHref);

  const auto content = zip.readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    Serial.printf("[%lu] [EBP] Failed to read item %s\n", millis(), path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize) const {
  const ZipFile zip("/sd" + filepath);
  const std::string path = FsHelpers::normalisePath(itemHref);

  return zip.readFileToStream(path.c_str(), out, chunkSize);
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const ZipFile zip("/sd" + filepath);
  return getItemSize(zip, itemHref, size);
}

bool Epub::getItemSize(const ZipFile& zip, const std::string& itemHref, size_t* size) {
  const std::string path = FsHelpers::normalisePath(itemHref);
  return zip.getInflatedFileSize(path.c_str(), size);
}

int Epub::getSpineItemsCount() const {
  if (!spineTocCache || !spineTocCache->isLoaded()) {
    return 0;
  }
  return spineTocCache->getSpineCount();
}

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const {
  if (!spineTocCache || !spineTocCache->isLoaded()) {
    Serial.printf("[%lu] [EBP] getCumulativeSpineItemSize called but cache not loaded\n", millis());
    return 0;
  }

  if (spineIndex < 0 || spineIndex >= spineTocCache->getSpineCount()) {
    Serial.printf("[%lu] [EBP] getCumulativeSpineItemSize index:%d is out of range\n", millis(), spineIndex);
    return 0;
  }

  return spineTocCache->getSpineEntry(spineIndex).cumulativeSize;
}

std::string Epub::getSpineHref(const int spineIndex) const {
  if (!spineTocCache || !spineTocCache->isLoaded()) {
    Serial.printf("[%lu] [EBP] getSpineItem called but cache not loaded\n", millis());
    return "";
  }

  if (spineIndex < 0 || spineIndex >= spineTocCache->getSpineCount()) {
    Serial.printf("[%lu] [EBP] getSpineItem index:%d is out of range\n", millis(), spineIndex);
    return spineTocCache->getSpineEntry(0).href;
  }

  return spineTocCache->getSpineEntry(spineIndex).href;
}

SpineTocCache::TocEntry Epub::getTocItem(const int tocIndex) const {
  if (!spineTocCache || !spineTocCache->isLoaded()) {
    Serial.printf("[%lu] [EBP] getTocItem called but cache not loaded\n", millis());
    return {};
  }

  if (tocIndex < 0 || tocIndex >= spineTocCache->getTocCount()) {
    Serial.printf("[%lu] [EBP] getTocItem index:%d is out of range\n", millis(), tocIndex);
    return {};
  }

  return spineTocCache->getTocEntry(tocIndex);
}

int Epub::getTocItemsCount() const {
  if (!spineTocCache || !spineTocCache->isLoaded()) {
    return 0;
  }

  return spineTocCache->getTocCount();
}

// work out the section index for a toc index
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  if (!spineTocCache || !spineTocCache->isLoaded()) {
    Serial.printf("[%lu] [EBP] getSpineIndexForTocIndex called but cache not loaded\n", millis());
    return 0;
  }

  if (tocIndex < 0 || tocIndex >= spineTocCache->getTocCount()) {
    Serial.printf("[%lu] [EBP] getSpineIndexForTocIndex: tocIndex %d out of range\n", millis(), tocIndex);
    return 0;
  }

  const int spineIndex = spineTocCache->getTocEntry(tocIndex).spineIndex;
  if (spineIndex < 0) {
    Serial.printf("[%lu] [EBP] Section not found for TOC index %d\n", millis(), tocIndex);
    return 0;
  }

  return spineIndex;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const {
  if (!spineTocCache || !spineTocCache->isLoaded()) {
    Serial.printf("[%lu] [EBP] getTocIndexForSpineIndex called but cache not loaded\n", millis());
    return -1;
  }

  if (spineIndex < 0 || spineIndex >= spineTocCache->getSpineCount()) {
    Serial.printf("[%lu] [EBP] getTocIndexForSpineIndex: spineIndex %d out of range\n", millis(), spineIndex);
    return -1;
  }

  return spineTocCache->getSpineEntry(spineIndex).tocIndex;
}

size_t Epub::getBookSize() const {
  if (!spineTocCache || !spineTocCache->isLoaded() || spineTocCache->getSpineCount() == 0) {
    return 0;
  }
  return getCumulativeSpineItemSize(getSpineItemsCount() - 1);
}

// Calculate progress in book
uint8_t Epub::calculateProgress(const int currentSpineIndex, const float currentSpineRead) const {
  const size_t bookSize = getBookSize();
  if (bookSize == 0) {
    return 0;
  }
  const size_t prevChapterSize = (currentSpineIndex >= 1) ? getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t curChapterSize = getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  const size_t sectionProgSize = currentSpineRead * curChapterSize;
  return round(static_cast<float>(prevChapterSize + sectionProgSize) / bookSize * 100.0);
}
