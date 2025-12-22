#include "SpineTocCache.h"

#include <HardwareSerial.h>
#include <SD.h>
#include <Serialization.h>
#include <ZipFile.h>

#include <vector>

#include "FsHelpers.h"

namespace {
constexpr uint8_t SPINE_TOC_CACHE_VERSION = 1;
constexpr char spineTocMetaBinFile[] = "/spine_toc_meta.bin";
constexpr char spineBinFile[] = "/spine.bin";
constexpr char tocBinFile[] = "/toc.bin";

bool openFileForRead(const std::string& path, File& file) {
  file = SD.open(path.c_str(), FILE_READ);
  if (!file) {
    Serial.printf("[%lu] [STC] Failed to open file for reading: %s\n", millis(), path.c_str());
    return false;
  }
  return true;
}

bool openFileForWrite(const std::string& path, File& file) {
  file = SD.open(path.c_str(), FILE_WRITE, true);
  if (!file) {
    Serial.printf("[%lu] [STC] Failed to open spine file for writing: %s\n", millis(), path.c_str());
    return false;
  }
  return true;
}
}  // namespace

bool SpineTocCache::beginWrite() {
  buildMode = true;
  spineCount = 0;
  tocCount = 0;

  Serial.printf("[%lu] [STC] Beginning write to cache path: %s\n", millis(), cachePath.c_str());

  // Open spine file for writing
  if (!openFileForWrite(cachePath + spineBinFile, spineFile)) {
    return false;
  }

  // Open TOC file for writing
  if (!openFileForWrite(cachePath + tocBinFile, tocFile)) {
    spineFile.close();
    return false;
  }

  Serial.printf("[%lu] [STC] Began writing cache files\n", millis());
  return true;
}

void SpineTocCache::writeSpineEntry(File& file, const SpineEntry& entry) const {
  serialization::writeString(file, entry.href);
  serialization::writePod(file, entry.cumulativeSize);
  serialization::writePod(file, entry.tocIndex);
}

void SpineTocCache::writeTocEntry(File& file, const TocEntry& entry) const {
  serialization::writeString(file, entry.title);
  serialization::writeString(file, entry.href);
  serialization::writeString(file, entry.anchor);
  serialization::writePod(file, entry.level);
  serialization::writePod(file, entry.spineIndex);
}

void SpineTocCache::addSpineEntry(const std::string& href) {
  if (!buildMode || !spineFile) {
    Serial.printf("[%lu] [STC] addSpineEntry called but not in build mode\n", millis());
    return;
  }

  const SpineEntry entry(href, 0, -1);
  writeSpineEntry(spineFile, entry);
  spineCount++;
}

void SpineTocCache::addTocEntry(const std::string& title, const std::string& href, const std::string& anchor,
                                const uint8_t level) {
  if (!buildMode || !tocFile) {
    Serial.printf("[%lu] [STC] addTocEntry called but not in build mode\n", millis());
    return;
  }

  const TocEntry entry(title, href, anchor, level, -1);
  writeTocEntry(tocFile, entry);
  tocCount++;
}

bool SpineTocCache::endWrite() {
  if (!buildMode) {
    Serial.printf("[%lu] [STC] endWrite called but not in build mode\n", millis());
    return false;
  }

  spineFile.close();
  tocFile.close();

  // Write metadata files with counts
  File metaFile;
  if (!openFileForWrite(cachePath + spineTocMetaBinFile, metaFile)) {
    return false;
  }
  serialization::writePod(metaFile, SPINE_TOC_CACHE_VERSION);
  serialization::writePod(metaFile, spineCount);
  serialization::writePod(metaFile, tocCount);
  metaFile.close();

  buildMode = false;
  Serial.printf("[%lu] [STC] Wrote %d spine, %d TOC entries\n", millis(), spineCount, tocCount);
  return true;
}

SpineTocCache::SpineEntry SpineTocCache::readSpineEntry(File& file) const {
  SpineEntry entry;
  serialization::readString(file, entry.href);
  serialization::readPod(file, entry.cumulativeSize);
  serialization::readPod(file, entry.tocIndex);
  return entry;
}

SpineTocCache::TocEntry SpineTocCache::readTocEntry(File& file) const {
  TocEntry entry;
  serialization::readString(file, entry.title);
  serialization::readString(file, entry.href);
  serialization::readString(file, entry.anchor);
  serialization::readPod(file, entry.level);
  serialization::readPod(file, entry.spineIndex);
  return entry;
}

bool SpineTocCache::updateMappingsAndSizes(const std::string& epubPath) {
  Serial.printf("[%lu] [STC] Computing mappings and sizes for %d spine, %d TOC entries\n", millis(), spineCount,
                tocCount);

  // Read all spine and TOC entries into temporary arrays (we need them all to compute mappings)
  // TODO: can we do this a bit smarter and avoid loading everything?
  std::vector<SpineEntry> spineEntries;
  std::vector<TocEntry> tocEntries;

  spineEntries.reserve(spineCount);
  tocEntries.reserve(tocCount);

  // Read spine entries
  {
    if (!openFileForRead(cachePath + spineBinFile, spineFile)) {
      return false;
    }
    for (int i = 0; i < spineCount; i++) {
      spineEntries.push_back(readSpineEntry(spineFile));
    }
    spineFile.close();
  }

  // Read TOC entries
  {
    if (!openFileForRead(cachePath + tocBinFile, tocFile)) {
      return false;
    }
    for (int i = 0; i < tocCount; i++) {
      tocEntries.push_back(readTocEntry(tocFile));
    }
    tocFile.close();
  }

  // Compute cumulative sizes
  const ZipFile zip("/sd" + epubPath);
  size_t cumSize = 0;

  for (int i = 0; i < spineCount; i++) {
    size_t itemSize = 0;
    const std::string path = FsHelpers::normalisePath(spineEntries[i].href);
    if (zip.getInflatedFileSize(path.c_str(), &itemSize)) {
      cumSize += itemSize;
      spineEntries[i].cumulativeSize = cumSize;
    } else {
      Serial.printf("[%lu] [STC] Warning: Could not get size for spine item: %s\n", millis(), path.c_str());
    }
  }

  Serial.printf("[%lu] [STC] Book size: %lu\n", millis(), cumSize);

  // Compute spine <-> TOC mappings
  for (int i = 0; i < spineCount; i++) {
    for (int j = 0; j < tocCount; j++) {
      if (tocEntries[j].href == spineEntries[i].href) {
        spineEntries[i].tocIndex = static_cast<int16_t>(j);
        tocEntries[j].spineIndex = static_cast<int16_t>(i);
        break;
      }
    }
  }

  // Rewrite spine file with updated data
  {
    if (!openFileForWrite(cachePath + spineBinFile, spineFile)) {
      return false;
    }
    for (const auto& entry : spineEntries) {
      writeSpineEntry(spineFile, entry);
    }
    spineFile.close();
  }

  // Rewrite TOC file with updated data
  {
    if (!openFileForWrite(cachePath + tocBinFile, tocFile)) {
      return false;
    }
    for (const auto& entry : tocEntries) {
      writeTocEntry(tocFile, entry);
    }
    tocFile.close();
  }

  // Clear vectors to free memory
  spineEntries.clear();
  spineEntries.shrink_to_fit();
  tocEntries.clear();
  tocEntries.shrink_to_fit();

  Serial.printf("[%lu] [STC] Updated cache with mappings and sizes\n", millis());
  return true;
}

bool SpineTocCache::load() {
  // Load metadata
  File metaFile;
  if (!openFileForRead(cachePath + spineTocMetaBinFile, metaFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(metaFile, version);
  if (version != SPINE_TOC_CACHE_VERSION) {
    Serial.printf("[%lu] [STC] Cache version mismatch: expected %d, got %d\n", millis(), SPINE_TOC_CACHE_VERSION,
                  version);
    metaFile.close();
    return false;
  }

  serialization::readPod(metaFile, spineCount);
  serialization::readPod(metaFile, tocCount);
  // TODO: Add LUT to back of meta file
  metaFile.close();

  loaded = true;
  Serial.printf("[%lu] [STC] Loaded cache metadata: %d spine, %d TOC entries\n", millis(), spineCount, tocCount);
  return true;
}

SpineTocCache::SpineEntry SpineTocCache::getSpineEntry(const int index) {
  if (!loaded) {
    Serial.printf("[%lu] [STC] getSpineEntry called but cache not loaded\n", millis());
    return {};
  }

  if (index < 0 || index >= static_cast<int>(spineCount)) {
    Serial.printf("[%lu] [STC] getSpineEntry index %d out of range\n", millis(), index);
    return {};
  }

  if (!openFileForRead(cachePath + spineBinFile, spineFile)) {
    return {};
  }

  // Seek to the correct entry - need to read entries sequentially until we reach the index
  // TODO: This could/should be based on a look up table/fixed sizes
  for (int i = 0; i < index; i++) {
    readSpineEntry(spineFile);  // Skip entries
  }

  auto entry = readSpineEntry(spineFile);
  spineFile.close();
  return entry;
}

SpineTocCache::TocEntry SpineTocCache::getTocEntry(const int index) {
  if (!loaded) {
    Serial.printf("[%lu] [STC] getTocEntry called but cache not loaded\n", millis());
    return {};
  }

  if (index < 0 || index >= static_cast<int>(tocCount)) {
    Serial.printf("[%lu] [STC] getTocEntry index %d out of range\n", millis(), index);
    return {};
  }

  if (!openFileForRead(cachePath + tocBinFile, tocFile)) {
    return {};
  }

  // Seek to the correct entry - need to read entries sequentially until we reach the index
  // TODO: This could/should be based on a look up table/fixed sizes
  for (int i = 0; i < index; i++) {
    readTocEntry(tocFile);  // Skip entries
  }

  auto entry = readTocEntry(tocFile);
  tocFile.close();
  return entry;
}

int SpineTocCache::getSpineCount() const { return spineCount; }

int SpineTocCache::getTocCount() const { return tocCount; }

bool SpineTocCache::isLoaded() const { return loaded; }
