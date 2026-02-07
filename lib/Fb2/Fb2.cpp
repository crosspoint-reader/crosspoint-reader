#include "Fb2.h"

#include <HalStorage.h>
#include <HardwareSerial.h>
#include <Serialization.h>

#include "Fb2/Fb2CoverExtractor.h"
#include "Fb2/Fb2MetadataParser.h"

namespace {
constexpr uint8_t FB2_CACHE_VERSION = 1;
}  // namespace

Fb2::Fb2(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
  cachePath = cacheDir + "/fb2_" + std::to_string(std::hash<std::string>{}(this->filepath));
}

bool Fb2::loadMetadataCache() {
  const auto cacheFile = cachePath + "/book.bin";
  FsFile file;
  if (!Storage.openFileForRead("FB2", cacheFile, file)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version != FB2_CACHE_VERSION) {
    file.close();
    Serial.printf("[%lu] [FB2] Cache version mismatch: %u vs %u\n", millis(), version, FB2_CACHE_VERSION);
    return false;
  }

  serialization::readString(file, title);
  serialization::readString(file, author);
  serialization::readString(file, language);
  serialization::readString(file, coverBinaryId);

  uint16_t sectionCount;
  serialization::readPod(file, sectionCount);
  sections.clear();
  sections.reserve(sectionCount);
  for (uint16_t i = 0; i < sectionCount; i++) {
    SectionInfo info;
    serialization::readString(file, info.title);
    uint32_t offset, length;
    serialization::readPod(file, offset);
    serialization::readPod(file, length);
    info.fileOffset = offset;
    info.length = length;
    sections.push_back(std::move(info));
  }

  uint16_t tocCount;
  serialization::readPod(file, tocCount);
  tocEntries.clear();
  tocEntries.reserve(tocCount);
  for (uint16_t i = 0; i < tocCount; i++) {
    TocEntry entry;
    serialization::readString(file, entry.title);
    int16_t idx;
    serialization::readPod(file, idx);
    entry.sectionIndex = idx;
    tocEntries.push_back(std::move(entry));
  }

  file.close();
  Serial.printf("[%lu] [FB2] Loaded metadata cache: %d sections, %d TOC entries\n", millis(), sectionCount, tocCount);
  return true;
}

bool Fb2::saveMetadataCache() const {
  const auto cacheFile = cachePath + "/book.bin";
  FsFile file;
  if (!Storage.openFileForWrite("FB2", cacheFile, file)) {
    return false;
  }

  serialization::writePod(file, FB2_CACHE_VERSION);
  serialization::writeString(file, title);
  serialization::writeString(file, author);
  serialization::writeString(file, language);
  serialization::writeString(file, coverBinaryId);

  const uint16_t sectionCount = static_cast<uint16_t>(sections.size());
  serialization::writePod(file, sectionCount);
  for (const auto& info : sections) {
    serialization::writeString(file, info.title);
    serialization::writePod(file, static_cast<uint32_t>(info.fileOffset));
    serialization::writePod(file, static_cast<uint32_t>(info.length));
  }

  const uint16_t tocCount = static_cast<uint16_t>(tocEntries.size());
  serialization::writePod(file, tocCount);
  for (const auto& entry : tocEntries) {
    serialization::writeString(file, entry.title);
    serialization::writePod(file, static_cast<int16_t>(entry.sectionIndex));
  }

  file.close();
  Serial.printf("[%lu] [FB2] Saved metadata cache\n", millis());
  return true;
}

bool Fb2::parseMetadata() {
  Fb2MetadataParser parser(filepath);
  if (!parser.parse()) {
    Serial.printf("[%lu] [FB2] Failed to parse metadata\n", millis());
    return false;
  }

  title = parser.getTitle();
  author = parser.getAuthor();
  language = parser.getLanguage();
  coverBinaryId = parser.getCoverBinaryId();
  sections = parser.getSections();
  tocEntries = parser.getTocEntries();

  Serial.printf("[%lu] [FB2] Parsed: title=%s, author=%s, sections=%d\n", millis(), title.c_str(), author.c_str(),
                static_cast<int>(sections.size()));
  return true;
}

bool Fb2::load(const bool buildIfMissing) {
  Serial.printf("[%lu] [FB2] Loading FB2: %s\n", millis(), filepath.c_str());

  // Try cache first
  if (loadMetadataCache()) {
    loaded = true;
    Serial.printf("[%lu] [FB2] Loaded from cache\n", millis());
    return true;
  }

  if (!buildIfMissing) {
    return false;
  }

  // Parse from scratch
  Serial.printf("[%lu] [FB2] Cache not found, parsing...\n", millis());
  setupCacheDir();

  if (!parseMetadata()) {
    return false;
  }

  if (!saveMetadataCache()) {
    Serial.printf("[%lu] [FB2] Warning: Could not save metadata cache\n", millis());
  }

  loaded = true;
  return true;
}

bool Fb2::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    Serial.printf("[%lu] [FB2] Cache does not exist, no action needed\n", millis());
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    Serial.printf("[%lu] [FB2] Failed to clear cache\n", millis());
    return false;
  }

  Serial.printf("[%lu] [FB2] Cache cleared successfully\n", millis());
  return true;
}

void Fb2::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }
  Storage.mkdir(cachePath.c_str());
}

const std::string& Fb2::getCachePath() const { return cachePath; }

const std::string& Fb2::getPath() const { return filepath; }

const std::string& Fb2::getTitle() const {
  static std::string blank;
  return loaded ? title : blank;
}

const std::string& Fb2::getAuthor() const {
  static std::string blank;
  return loaded ? author : blank;
}

const std::string& Fb2::getLanguage() const {
  static std::string blank;
  return loaded ? language : blank;
}

std::string Fb2::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Fb2::generateCoverBmp() const {
  if (Storage.exists(getCoverBmpPath().c_str())) {
    return true;
  }

  if (!loaded || coverBinaryId.empty()) {
    Serial.printf("[%lu] [FB2] No cover image available\n", millis());
    return false;
  }

  setupCacheDir();
  Fb2CoverExtractor extractor(filepath, coverBinaryId, getCoverBmpPath());
  return extractor.extract();
}

std::string Fb2::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }

std::string Fb2::getThumbBmpPath(int height) const { return cachePath + "/thumb_" + std::to_string(height) + ".bmp"; }

bool Fb2::generateThumbBmp(int height) const {
  if (Storage.exists(getThumbBmpPath(height).c_str())) {
    return true;
  }

  if (!loaded || coverBinaryId.empty()) {
    Serial.printf("[%lu] [FB2] No cover image for thumbnail\n", millis());
    // Write empty file to avoid future attempts
    FsFile thumbBmp;
    setupCacheDir();
    Storage.openFileForWrite("FB2", getThumbBmpPath(height), thumbBmp);
    thumbBmp.close();
    return false;
  }

  setupCacheDir();
  Fb2CoverExtractor extractor(filepath, coverBinaryId, "");
  return extractor.extractThumb(getThumbBmpPath(height), height);
}

int Fb2::getSectionCount() const { return static_cast<int>(sections.size()); }

const Fb2::SectionInfo& Fb2::getSectionInfo(int index) const {
  static SectionInfo empty = {"", 0, 0};
  if (index < 0 || index >= static_cast<int>(sections.size())) {
    return empty;
  }
  return sections[index];
}

size_t Fb2::getBookSize() const {
  if (sections.empty()) {
    return 0;
  }
  return getCumulativeSectionSize(static_cast<int>(sections.size()) - 1);
}

size_t Fb2::getCumulativeSectionSize(int index) const {
  if (index < 0 || index >= static_cast<int>(sections.size())) {
    return 0;
  }
  size_t cumulative = 0;
  for (int i = 0; i <= index; i++) {
    cumulative += sections[i].length;
  }
  return cumulative;
}

float Fb2::calculateProgress(int currentSectionIndex, float currentSectionRead) const {
  const size_t bookSize = getBookSize();
  if (bookSize == 0) {
    return 0.0f;
  }
  const size_t prevSize = (currentSectionIndex >= 1) ? getCumulativeSectionSize(currentSectionIndex - 1) : 0;
  const size_t curSize = getCumulativeSectionSize(currentSectionIndex) - prevSize;
  const float sectionProgSize = currentSectionRead * static_cast<float>(curSize);
  const float totalProgress = static_cast<float>(prevSize) + sectionProgSize;
  return totalProgress / static_cast<float>(bookSize);
}

int Fb2::getTocCount() const { return static_cast<int>(tocEntries.size()); }

const Fb2::TocEntry& Fb2::getTocEntry(int index) const {
  static TocEntry empty = {"", -1};
  if (index < 0 || index >= static_cast<int>(tocEntries.size())) {
    return empty;
  }
  return tocEntries[index];
}

int Fb2::getTocIndexForSectionIndex(int sectionIndex) const {
  for (int i = 0; i < static_cast<int>(tocEntries.size()); i++) {
    if (tocEntries[i].sectionIndex == sectionIndex) {
      return i;
    }
  }
  // Return closest lower TOC entry
  int best = -1;
  for (int i = 0; i < static_cast<int>(tocEntries.size()); i++) {
    if (tocEntries[i].sectionIndex <= sectionIndex) {
      best = i;
    }
  }
  return best;
}

int Fb2::getSectionIndexForTocIndex(int tocIndex) const {
  if (tocIndex < 0 || tocIndex >= static_cast<int>(tocEntries.size())) {
    return 0;
  }
  return tocEntries[tocIndex].sectionIndex;
}
