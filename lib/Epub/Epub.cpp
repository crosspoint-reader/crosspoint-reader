#include "Epub.h"

#include <FsHelpers.h>
#include <HardwareSerial.h>
#include <JpegToBmpConverter.h>
#include <SDCardManager.h>
#include <ZipFile.h>

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

bool Epub::parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata) {
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

  ContentOpfParser opfParser(getCachePath(), getBasePath(), contentOpfSize, bookMetadataCache.get());
  if (!opfParser.setup()) {
    Serial.printf("[%lu] [EBP] Could not setup content.opf parser\n", millis());
    return false;
  }

  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024)) {
    Serial.printf("[%lu] [EBP] Could not read content.opf\n", millis());
    return false;
  }

  // Grab data from opfParser into epub
  bookMetadata.title = opfParser.title;
  bookMetadata.author = opfParser.author;
  bookMetadata.coverItemHref = opfParser.coverItemHref;
  bookMetadata.textReferenceHref = opfParser.textReferenceHref;

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
  FsFile tempNcxFile;
  if (!SdMan.openFileForWrite("EBP", tmpNcxPath, tempNcxFile)) {
    return false;
  }
  readItemContentsToStream(tocNcxItem, tempNcxFile, 1024);
  tempNcxFile.close();
  if (!SdMan.openFileForRead("EBP", tmpNcxPath, tempNcxFile)) {
    return false;
  }
  const auto ncxSize = tempNcxFile.size();

  TocNcxParser ncxParser(contentBasePath, ncxSize, bookMetadataCache.get());

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
  SdMan.remove(tmpNcxPath.c_str());

  Serial.printf("[%lu] [EBP] Parsed TOC items\n", millis());
  return true;
}

// load in the meta data for the epub file
bool Epub::load() {
  Serial.printf("[%lu] [EBP] Loading ePub: %s\n", millis(), filepath.c_str());

  // Initialize spine/TOC cache
  bookMetadataCache.reset(new BookMetadataCache(cachePath));

  // Try to load existing cache first
  if (bookMetadataCache->load()) {
    Serial.printf("[%lu] [EBP] Loaded ePub: %s\n", millis(), filepath.c_str());
    return true;
  }

  // Cache doesn't exist or is invalid, build it
  Serial.printf("[%lu] [EBP] Cache not found, building spine/TOC cache\n", millis());
  setupCacheDir();

  // Begin building cache - stream entries to disk immediately
  if (!bookMetadataCache->beginWrite()) {
    Serial.printf("[%lu] [EBP] Could not begin writing cache\n", millis());
    return false;
  }

  // OPF Pass
  BookMetadataCache::BookMetadata bookMetadata;
  if (!bookMetadataCache->beginContentOpfPass()) {
    Serial.printf("[%lu] [EBP] Could not begin writing content.opf pass\n", millis());
    return false;
  }
  if (!parseContentOpf(bookMetadata)) {
    Serial.printf("[%lu] [EBP] Could not parse content.opf\n", millis());
    return false;
  }
  if (!bookMetadataCache->endContentOpfPass()) {
    Serial.printf("[%lu] [EBP] Could not end writing content.opf pass\n", millis());
    return false;
  }

  // TOC Pass
  if (!bookMetadataCache->beginTocPass()) {
    Serial.printf("[%lu] [EBP] Could not begin writing toc pass\n", millis());
    return false;
  }
  if (!parseTocNcxFile()) {
    Serial.printf("[%lu] [EBP] Could not parse toc\n", millis());
    return false;
  }
  if (!bookMetadataCache->endTocPass()) {
    Serial.printf("[%lu] [EBP] Could not end writing toc pass\n", millis());
    return false;
  }

  // Close the cache files
  if (!bookMetadataCache->endWrite()) {
    Serial.printf("[%lu] [EBP] Could not end writing cache\n", millis());
    return false;
  }

  // Build final book.bin
  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    Serial.printf("[%lu] [EBP] Could not update mappings and sizes\n", millis());
    return false;
  }

  if (!bookMetadataCache->cleanupTmpFiles()) {
    Serial.printf("[%lu] [EBP] Could not cleanup tmp files - ignoring\n", millis());
  }

  // Reload the cache from disk so it's in the correct state
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  if (!bookMetadataCache->load()) {
    Serial.printf("[%lu] [EBP] Failed to reload cache after writing\n", millis());
    return false;
  }

  Serial.printf("[%lu] [EBP] Loaded ePub: %s\n", millis(), filepath.c_str());
  return true;
}

bool Epub::clearCache() const {
  if (!SdMan.exists(cachePath.c_str())) {
    Serial.printf("[%lu] [EPB] Cache does not exist, no action needed\n", millis());
    return true;
  }

  if (!SdMan.removeDir(cachePath.c_str())) {
    Serial.printf("[%lu] [EPB] Failed to clear cache\n", millis());
    return false;
  }

  Serial.printf("[%lu] [EPB] Cache cleared successfully\n", millis());
  return true;
}

void Epub::setupCacheDir() const {
  if (SdMan.exists(cachePath.c_str())) {
    return;
  }

  SdMan.mkdir(cachePath.c_str());
}

const std::string& Epub::getCachePath() const { return cachePath; }

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.title;
}

const std::string& Epub::getAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.author;
}

std::string Epub::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Epub::generateCoverBmp() const {
  // Already generated, return true
  if (SdMan.exists(getCoverBmpPath().c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    Serial.printf("[%lu] [EBP] Cannot generate cover BMP, cache not loaded\n", millis());
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    Serial.printf("[%lu] [EBP] No known cover image\n", millis());
    return false;
  }

  if (coverImageHref.substr(coverImageHref.length() - 4) == ".jpg" ||
      coverImageHref.substr(coverImageHref.length() - 5) == ".jpeg") {
    Serial.printf("[%lu] [EBP] Generating BMP from JPG cover image\n", millis());
    const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";

    FsFile coverJpg;
    if (!SdMan.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }
    readItemContentsToStream(coverImageHref, coverJpg, 1024);
    coverJpg.close();

    if (!SdMan.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }

    FsFile coverBmp;
    if (!SdMan.openFileForWrite("EBP", getCoverBmpPath(), coverBmp)) {
      coverJpg.close();
      return false;
    }
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp);
    coverJpg.close();
    coverBmp.close();
    SdMan.remove(coverJpgTempPath.c_str());

    if (!success) {
      Serial.printf("[%lu] [EBP] Failed to generate BMP from JPG cover image\n", millis());
      SdMan.remove(getCoverBmpPath().c_str());
    }
    Serial.printf("[%lu] [EBP] Generated BMP from JPG cover image, success: %s\n", millis(), success ? "yes" : "no");
    return success;
  } else {
    Serial.printf("[%lu] [EBP] Cover image is not a JPG, skipping\n", millis());
  }

  return false;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, const bool trailingNullByte) const {
  if (itemHref.empty()) {
    Serial.printf("[%lu] [EBP] Failed to read item, empty href\n", millis());
    return nullptr;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);

  const auto content = ZipFile(filepath).readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    Serial.printf("[%lu] [EBP] Failed to read item %s\n", millis(), path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize) const {
  if (itemHref.empty()) {
    Serial.printf("[%lu] [EBP] Failed to read item, empty href\n", millis());
    return false;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).readFileToStream(path.c_str(), out, chunkSize);
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).getInflatedFileSize(path.c_str(), size);
}

int Epub::getSpineItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  return bookMetadataCache->getSpineCount();
}

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const { return getSpineItem(spineIndex).cumulativeSize; }

BookMetadataCache::SpineEntry Epub::getSpineItem(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    Serial.printf("[%lu] [EBP] getSpineItem called but cache not loaded\n", millis());
    return {};
  }

  if (spineIndex < 0 || spineIndex >= bookMetadataCache->getSpineCount()) {
    Serial.printf("[%lu] [EBP] getSpineItem index:%d is out of range\n", millis(), spineIndex);
    return bookMetadataCache->getSpineEntry(0);
  }

  return bookMetadataCache->getSpineEntry(spineIndex);
}

BookMetadataCache::TocEntry Epub::getTocItem(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    Serial.printf("[%lu] [EBP] getTocItem called but cache not loaded\n", millis());
    return {};
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    Serial.printf("[%lu] [EBP] getTocItem index:%d is out of range\n", millis(), tocIndex);
    return {};
  }

  return bookMetadataCache->getTocEntry(tocIndex);
}

int Epub::getTocItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }

  return bookMetadataCache->getTocCount();
}

// work out the section index for a toc index
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    Serial.printf("[%lu] [EBP] getSpineIndexForTocIndex called but cache not loaded\n", millis());
    return 0;
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    Serial.printf("[%lu] [EBP] getSpineIndexForTocIndex: tocIndex %d out of range\n", millis(), tocIndex);
    return 0;
  }

  const int spineIndex = bookMetadataCache->getTocEntry(tocIndex).spineIndex;
  if (spineIndex < 0) {
    Serial.printf("[%lu] [EBP] Section not found for TOC index %d\n", millis(), tocIndex);
    return 0;
  }

  return spineIndex;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const { return getSpineItem(spineIndex).tocIndex; }

size_t Epub::getBookSize() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || bookMetadataCache->getSpineCount() == 0) {
    return 0;
  }
  return getCumulativeSpineItemSize(getSpineItemsCount() - 1);
}

int Epub::getSpineIndexForTextReference() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    Serial.printf("[%lu] [EBP] getSpineIndexForTextReference called but cache not loaded\n", millis());
    return 0;
  }
  Serial.printf("[%lu] [ERS] Core Metadata: cover(%d)=%s, textReference(%d)=%s\n", millis(),
                bookMetadataCache->coreMetadata.coverItemHref.size(),
                bookMetadataCache->coreMetadata.coverItemHref.c_str(),
                bookMetadataCache->coreMetadata.textReferenceHref.size(),
                bookMetadataCache->coreMetadata.textReferenceHref.c_str());

  if (bookMetadataCache->coreMetadata.textReferenceHref.empty()) {
    // there was no textReference in epub, so we return 0 (the first chapter)
    return 0;
  }

  // loop through spine items to get the correct index matching the text href
  for (size_t i = 0; i < getSpineItemsCount(); i++) {
    if (getSpineItem(i).href == bookMetadataCache->coreMetadata.textReferenceHref) {
      Serial.printf("[%lu] [ERS] Text reference %s found at index %d\n", millis(),
                    bookMetadataCache->coreMetadata.textReferenceHref.c_str(), i);
      return i;
    }
  }
  // This should not happen, as we checked for empty textReferenceHref earlier
  Serial.printf("[%lu] [EBP] Section not found for text reference\n", millis());
  return 0;
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
