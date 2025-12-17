#include "Epub.h"

#include <HardwareSerial.h>
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
    containerParser.teardown();
    return false;
  }

  // Extract the result
  if (containerParser.fullPath.empty()) {
    Serial.printf("[%lu] [EBP] Could not find valid rootfile in container.xml\n", millis());
    containerParser.teardown();
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);

  containerParser.teardown();
  return true;
}

bool Epub::parseContentOpf(const std::string& contentOpfFilePath) {
  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    Serial.printf("[%lu] [EBP] Could not get size of content.opf\n", millis());
    return false;
  }

  ContentOpfParser opfParser(getBasePath(), contentOpfSize);

  if (!opfParser.setup()) {
    Serial.printf("[%lu] [EBP] Could not setup content.opf parser\n", millis());
    return false;
  }

  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024)) {
    Serial.printf("[%lu] [EBP] Could not read content.opf\n", millis());
    opfParser.teardown();
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

  for (auto& spineRef : opfParser.spineRefs) {
    if (opfParser.items.count(spineRef)) {
      spine.emplace_back(spineRef, opfParser.items.at(spineRef));
    }
  }

  Serial.printf("[%lu] [EBP] Successfully parsed content.opf\n", millis());

  opfParser.teardown();
  return true;
}

bool Epub::parseTocNcxFile() {
  // the ncx file should have been specified in the content.opf file
  if (tocNcxItem.empty()) {
    Serial.printf("[%lu] [EBP] No ncx file specified\n", millis());
    return false;
  }

  size_t tocSize;
  if (!getItemSize(tocNcxItem, &tocSize)) {
    Serial.printf("[%lu] [EBP] Could not get size of toc ncx\n", millis());
    return false;
  }

  TocNcxParser ncxParser(contentBasePath, tocSize);

  if (!ncxParser.setup()) {
    Serial.printf("[%lu] [EBP] Could not setup toc ncx parser\n", millis());
    return false;
  }

  if (!readItemContentsToStream(tocNcxItem, ncxParser, 1024)) {
    Serial.printf("[%lu] [EBP] Could not read toc ncx stream\n", millis());
    ncxParser.teardown();
    return false;
  }

  this->toc = std::move(ncxParser.toc);

  Serial.printf("[%lu] [EBP] Parsed %d TOC items\n", millis(), this->toc.size());

  ncxParser.teardown();
  return true;
}

// load in the meta data for the epub file
bool Epub::load() {
  Serial.printf("[%lu] [EBP] Loading ePub: %s\n", millis(), filepath.c_str());
  ZipFile zip("/sd" + filepath);

  if (!footnotePages) {
    footnotePages = new std::unordered_set<std::string>();
  }

  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    Serial.printf("[%lu] [EBP] Could not find content.opf in zip\n", millis());
    return false;
  }

  Serial.printf("[%lu] [EBP] Found content.opf at: %s\n", millis(), contentOpfFilePath.c_str());

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  if (!parseContentOpf(contentOpfFilePath)) {
    Serial.printf("[%lu] [EBP] Could not parse content.opf\n", millis());
    return false;
  }

  if (!parseTocNcxFile()) {
    Serial.printf("[%lu] [EBP] Could not parse toc\n", millis());
    return false;
  }

  // determine size of spine items
  size_t spineItemsCount = getSpineItemsCount();
  size_t spineItemsSize = 0;
  for (size_t i = 0; i < spineItemsCount; i++) {
    std::string spineItem = getSpineItem(i);
    size_t s = 0;
    getItemSize(spineItem, &s);
    spineItemsSize += s;
    cumulativeSpineItemSize.emplace_back(spineItemsSize);
  }
  Serial.printf("[%lu] [EBP] Book size: %u\n", millis(), spineItemsSize);

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

const std::string& Epub::getCoverImageItem() const { return coverImageItem; }

std::string normalisePath(const std::string& path) {
  std::vector<std::string> components;
  std::string component;

  for (const auto c : path) {
    if (c == '/') {
      if (!component.empty()) {
        if (component == "..") {
          if (!components.empty()) {
            components.pop_back();
          }
        } else {
          components.push_back(component);
        }
        component.clear();
      }
    } else {
      component += c;
    }
  }

  if (!component.empty()) {
    components.push_back(component);
  }

  std::string result;
  for (const auto& c : components) {
    if (!result.empty()) {
      result += "/";
    }
    result += c;
  }

  return result;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, bool trailingNullByte) const {
  const ZipFile zip("/sd" + filepath);
  const std::string path = normalisePath(itemHref);

  const auto content = zip.readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    Serial.printf("[%lu] [EBP] Failed to read item %s\n", millis(), path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize) const {
  const ZipFile zip("/sd" + filepath);
  const std::string path = normalisePath(itemHref);

  return zip.readFileToStream(path.c_str(), out, chunkSize);
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const ZipFile zip("/sd" + filepath);
  const std::string path = normalisePath(itemHref);

  return zip.getInflatedFileSize(path.c_str(), size);
}

int Epub::getSpineItemsCount() const {
  int virtualCount = virtualSpineItems ? virtualSpineItems->size() : 0;
  return spine.size() + virtualCount;
}

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const { return cumulativeSpineItemSize.at(spineIndex); }

std::string Epub::getSpineItem(const int spineIndex) const {
  // Normal spine item
  if (spineIndex >= 0 && spineIndex < static_cast<int>(spine.size())) {
    return contentBasePath + spine.at(spineIndex).second;
  }

  // Virtual spine item
  if (virtualSpineItems) {
    int virtualIndex = spineIndex - spine.size();
    if (virtualIndex >= 0 && virtualIndex < static_cast<int>(virtualSpineItems->size())) {
      return (*virtualSpineItems)[virtualIndex];
    }
  }

  Serial.printf("[%lu] [EBP] getSpineItem index:%d is out of range\n", millis(), spineIndex);

  // Return empty string instead of reference to avoid issues
  static std::string emptyString = "";
  return emptyString;
}

EpubTocEntry& Epub::getTocItem(const int tocTndex) {
  if (tocTndex < 0 || tocTndex >= toc.size()) {
    Serial.printf("[%lu] [EBP] getTocItem index:%d is out of range\n", millis(), tocTndex);
    return toc.at(0);
  }

  return toc.at(tocTndex);
}

int Epub::getTocItemsCount() const { return toc.size(); }

// work out the section index for a toc index
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  // the toc entry should have an href that matches the spine item
  // so we can find the spine index by looking for the href
  for (int i = 0; i < spine.size(); i++) {
    if (spine[i].second == toc[tocIndex].href) {
      return i;
    }
  }

  Serial.printf("[%lu] [EBP] Section not found\n", millis());
  // not found - default to the start of the book
  return 0;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const {
  // Skip virtual spine items
  if (isVirtualSpineItem(spineIndex)) {
    return -1;
  }

  // the toc entry should have an href that matches the spine item
  // so we can find the toc index by looking for the href
  for (int i = 0; i < toc.size(); i++) {
    if (toc[i].href == spine[spineIndex].second) {
      return i;
    }
  }

  Serial.printf("[%lu] [EBP] TOC item not found\n", millis());
  return -1;
}

void Epub::markAsFootnotePage(const std::string& href) {
  // Lazy initialization
  if (!footnotePages) {
    footnotePages = new std::unordered_set<std::string>();
  }

  // Extract filename from href (remove #anchor if present)
  size_t hashPos = href.find('#');
  std::string filename = (hashPos != std::string::npos) ? href.substr(0, hashPos) : href;

  // Extract just the filename without path
  size_t lastSlash = filename.find_last_of('/');
  if (lastSlash != std::string::npos) {
    filename = filename.substr(lastSlash + 1);
  }

  footnotePages->insert(filename);
  Serial.printf("[%lu] [EPUB] Marked as footnote page: %s\n", millis(), filename.c_str());
}

bool Epub::isFootnotePage(const std::string& filename) const {
  if (!footnotePages) return false;
  return footnotePages->find(filename) != footnotePages->end();
}

bool Epub::shouldHideFromToc(int spineIndex) const {
  // Always hide virtual spine items
  if (isVirtualSpineItem(spineIndex)) {
    return true;
  }

  if (spineIndex < 0 || spineIndex >= spine.size()) {
    return true;
  }

  const std::string& spineItem = spine[spineIndex].second;

  // Extract filename from spine item
  size_t lastSlash = spineItem.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? spineItem.substr(lastSlash + 1) : spineItem;

  return isFootnotePage(filename);
}

// Virtual spine items
int Epub::addVirtualSpineItem(const std::string& path) {
  // Lazy initialization
  if (!virtualSpineItems) {
    virtualSpineItems = new std::vector<std::string>();
  }

  virtualSpineItems->push_back(path);
  int newIndex = spine.size() + virtualSpineItems->size() - 1;
  Serial.printf("[%lu] [EPUB] Added virtual spine item: %s (index %d)\n", millis(), path.c_str(), newIndex);
  return newIndex;
}

bool Epub::isVirtualSpineItem(int spineIndex) const { return spineIndex >= static_cast<int>(spine.size()); }

int Epub::findVirtualSpineIndex(const std::string& filename) const {
  if (!virtualSpineItems) return -1;

  for (size_t i = 0; i < virtualSpineItems->size(); i++) {
    std::string virtualPath = (*virtualSpineItems)[i];
    size_t lastSlash = virtualPath.find_last_of('/');
    std::string virtualFilename = (lastSlash != std::string::npos) ? virtualPath.substr(lastSlash + 1) : virtualPath;

    if (virtualFilename == filename) {
      return spine.size() + i;
    }
  }
  return -1;
}
size_t Epub::getBookSize() const { return getCumulativeSpineItemSize(getSpineItemsCount() - 1); }

// Calculate progress in book
uint8_t Epub::calculateProgress(const int currentSpineIndex, const float currentSpineRead) {
  size_t prevChapterSize = (currentSpineIndex >= 1) ? getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  size_t curChapterSize = getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  size_t bookSize = getBookSize();
  size_t sectionProgSize = currentSpineRead * curChapterSize;
  return round(static_cast<float>(prevChapterSize + sectionProgSize) / bookSize * 100.0);
}
