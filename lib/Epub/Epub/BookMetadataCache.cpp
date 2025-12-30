#include "BookMetadataCache.h"

#include <HardwareSerial.h>
#include <Serialization.h>
#include <ZipFile.h>

#include <vector>

#include "FsHelpers.h"

namespace {
constexpr uint8_t BOOK_CACHE_VERSION = 1;
constexpr char bookBinFile[] = "/book.bin";
constexpr char tmpSpineBinFile[] = "/spine.bin.tmp";
constexpr char tmpTocBinFile[] = "/toc.bin.tmp";
}  // namespace

/* ============= WRITING / BUILDING FUNCTIONS ================ */

bool BookMetadataCache::beginWrite() {
  buildMode = true;
  spineCount = 0;
  tocCount = 0;
  Serial.printf("[%lu] [BMC] Entering write mode\n", millis());
  return true;
}

bool BookMetadataCache::beginContentOpfPass() {
  Serial.printf("[%lu] [BMC] Beginning content opf pass\n", millis());

  // Open spine file for writing
  return SdMan.openFileForWrite("BMC", cachePath + tmpSpineBinFile, spineFile);
}

bool BookMetadataCache::endContentOpfPass() {
  spineFile.close();
  return true;
}

bool BookMetadataCache::beginTocPass() {
  Serial.printf("[%lu] [BMC] Beginning toc pass\n", millis());

  // Open spine file for reading
  if (!SdMan.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  if (!SdMan.openFileForWrite("BMC", cachePath + tmpTocBinFile, tocFile)) {
    spineFile.close();
    return false;
  }
  return true;
}

bool BookMetadataCache::endTocPass() {
  tocFile.close();
  spineFile.close();
  return true;
}

bool BookMetadataCache::endWrite() {
  if (!buildMode) {
    Serial.printf("[%lu] [BMC] endWrite called but not in build mode\n", millis());
    return false;
  }

  buildMode = false;
  Serial.printf("[%lu] [BMC] Wrote %d spine, %d TOC entries\n", millis(), spineCount, tocCount);
  return true;
}

bool BookMetadataCache::buildBookBin(const std::string& epubPath, const BookMetadata& metadata) {
  // Open all three files, writing to meta, reading from spine and toc
  if (!SdMan.openFileForWrite("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  if (!SdMan.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    bookFile.close();
    return false;
  }

  if (!SdMan.openFileForRead("BMC", cachePath + tmpTocBinFile, tocFile)) {
    bookFile.close();
    spineFile.close();
    return false;
  }

  constexpr uint32_t headerASize =
      sizeof(BOOK_CACHE_VERSION) + /* LUT Offset */ sizeof(uint32_t) + sizeof(spineCount) + sizeof(tocCount);
  const uint32_t metadataSize =
      metadata.title.size() + metadata.author.size() + metadata.coverItemHref.size() + sizeof(uint32_t) * 3;
  const uint32_t lutSize = sizeof(uint32_t) * spineCount + sizeof(uint32_t) * tocCount;
  const uint32_t lutOffset = headerASize + metadataSize;

  // Header A
  serialization::writePod(bookFile, BOOK_CACHE_VERSION);
  serialization::writePod(bookFile, lutOffset);
  serialization::writePod(bookFile, spineCount);
  serialization::writePod(bookFile, tocCount);
  // Metadata
  serialization::writeString(bookFile, metadata.title);
  serialization::writeString(bookFile, metadata.author);
  serialization::writeString(bookFile, metadata.coverItemHref);

  // Loop through spine entries, writing LUT positions
  spineFile.seek(0);
  for (int i = 0; i < spineCount; i++) {
    uint32_t pos = spineFile.position();
    auto spineEntry = readSpineEntry(spineFile);
    serialization::writePod(bookFile, pos + lutOffset + lutSize);
  }

  // Loop through toc entries, writing LUT positions
  tocFile.seek(0);
  for (int i = 0; i < tocCount; i++) {
    uint32_t pos = tocFile.position();
    auto tocEntry = readTocEntry(tocFile);
    serialization::writePod(bookFile, pos + lutOffset + lutSize + static_cast<uint32_t>(spineFile.position()));
  }

  // LUTs complete
  // Loop through spines from spine file matching up TOC indexes, calculating cumulative size and writing to book.bin

  ZipFile zip(epubPath);
  // Pre-open zip file to speed up size calculations
  if (!zip.open()) {
    Serial.printf("[%lu] [BMC] Could not open EPUB zip for size calculations\n", millis());
    bookFile.close();
    spineFile.close();
    tocFile.close();
    return false;
  }
  // TODO: For large ZIPs loading the all localHeaderOffsets will crash.
  //       However not having them loaded is extremely slow. Need a better solution here.
  //       Perhaps only a cache of spine items or a better way to speedup lookups?
  if (!zip.loadAllFileStatSlims()) {
    Serial.printf("[%lu] [BMC] Could not load zip local header offsets for size calculations\n", millis());
    bookFile.close();
    spineFile.close();
    tocFile.close();
    zip.close();
    return false;
  }
  uint32_t cumSize = 0;
  spineFile.seek(0);
  for (int i = 0; i < spineCount; i++) {
    auto spineEntry = readSpineEntry(spineFile);

    tocFile.seek(0);
    for (int j = 0; j < tocCount; j++) {
      auto tocEntry = readTocEntry(tocFile);
      if (tocEntry.spineIndex == i) {
        spineEntry.tocIndex = j;
        break;
      }
    }

    // Not a huge deal if we don't fine a TOC entry for the spine entry, this is expected behaviour for EPUBs
    // Logging here is for debugging
    if (spineEntry.tocIndex == -1) {
      Serial.printf("[%lu] [BMC] Warning: Could not find TOC entry for spine item %d: %s\n", millis(), i,
                    spineEntry.href.c_str());
    }

    // Calculate size for cumulative size
    size_t itemSize = 0;
    const std::string path = FsHelpers::normalisePath(spineEntry.href);
    if (zip.getInflatedFileSize(path.c_str(), &itemSize)) {
      cumSize += itemSize;
      spineEntry.cumulativeSize = cumSize;
    } else {
      Serial.printf("[%lu] [BMC] Warning: Could not get size for spine item: %s\n", millis(), path.c_str());
    }

    // Write out spine data to book.bin
    writeSpineEntry(bookFile, spineEntry);
  }
  // Close opened zip file
  zip.close();

  // Loop through toc entries from toc file writing to book.bin
  tocFile.seek(0);
  for (int i = 0; i < tocCount; i++) {
    auto tocEntry = readTocEntry(tocFile);
    writeTocEntry(bookFile, tocEntry);
  }

  bookFile.close();
  spineFile.close();
  tocFile.close();

  Serial.printf("[%lu] [BMC] Successfully built book.bin\n", millis());
  return true;
}

bool BookMetadataCache::cleanupTmpFiles() const {
  if (SdMan.exists((cachePath + tmpSpineBinFile).c_str())) {
    SdMan.remove((cachePath + tmpSpineBinFile).c_str());
  }
  if (SdMan.exists((cachePath + tmpTocBinFile).c_str())) {
    SdMan.remove((cachePath + tmpTocBinFile).c_str());
  }
  return true;
}

uint32_t BookMetadataCache::writeSpineEntry(FsFile& file, const SpineEntry& entry) const {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.href);
  serialization::writePod(file, entry.cumulativeSize);
  serialization::writePod(file, entry.tocIndex);
  return pos;
}

uint32_t BookMetadataCache::writeTocEntry(FsFile& file, const TocEntry& entry) const {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.title);
  serialization::writeString(file, entry.href);
  serialization::writeString(file, entry.anchor);
  serialization::writePod(file, entry.level);
  serialization::writePod(file, entry.spineIndex);
  return pos;
}

// Note: for the LUT to be accurate, this **MUST** be called for all spine items before `addTocEntry` is ever called
// this is because in this function we're marking positions of the items
void BookMetadataCache::createSpineEntry(const std::string& href) {
  if (!buildMode || !spineFile) {
    Serial.printf("[%lu] [BMC] createSpineEntry called but not in build mode\n", millis());
    return;
  }

  const SpineEntry entry(href, 0, -1);
  writeSpineEntry(spineFile, entry);
  spineCount++;
}

void BookMetadataCache::createTocEntry(const std::string& title, const std::string& href, const std::string& anchor,
                                       const uint8_t level) {
  if (!buildMode || !tocFile || !spineFile) {
    Serial.printf("[%lu] [BMC] createTocEntry called but not in build mode\n", millis());
    return;
  }

  int spineIndex = -1;
  // find spine index
  // TODO: This lookup is slow as need to scan through all items each time. We can't hold it all in memory due to size.
  //       But perhaps we can load just the hrefs in a vector/list to do an index lookup?
  spineFile.seek(0);
  for (int i = 0; i < spineCount; i++) {
    auto spineEntry = readSpineEntry(spineFile);
    if (spineEntry.href == href) {
      spineIndex = i;
      break;
    }
  }

  if (spineIndex == -1) {
    Serial.printf("[%lu] [BMC] addTocEntry: Could not find spine item for TOC href %s\n", millis(), href.c_str());
  }

  const TocEntry entry(title, href, anchor, level, spineIndex);
  writeTocEntry(tocFile, entry);
  tocCount++;
}

/* ============= READING / LOADING FUNCTIONS ================ */

bool BookMetadataCache::load() {
  if (!SdMan.openFileForRead("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(bookFile, version);
  if (version != BOOK_CACHE_VERSION) {
    Serial.printf("[%lu] [BMC] Cache version mismatch: expected %d, got %d\n", millis(), BOOK_CACHE_VERSION, version);
    bookFile.close();
    return false;
  }

  serialization::readPod(bookFile, lutOffset);
  serialization::readPod(bookFile, spineCount);
  serialization::readPod(bookFile, tocCount);

  serialization::readString(bookFile, coreMetadata.title);
  serialization::readString(bookFile, coreMetadata.author);
  serialization::readString(bookFile, coreMetadata.coverItemHref);

  loaded = true;
  Serial.printf("[%lu] [BMC] Loaded cache data: %d spine, %d TOC entries\n", millis(), spineCount, tocCount);
  return true;
}

BookMetadataCache::SpineEntry BookMetadataCache::getSpineEntry(const int index) {
  if (!loaded) {
    Serial.printf("[%lu] [BMC] getSpineEntry called but cache not loaded\n", millis());
    return {};
  }

  if (index < 0 || index >= static_cast<int>(spineCount)) {
    Serial.printf("[%lu] [BMC] getSpineEntry index %d out of range\n", millis(), index);
    return {};
  }

  // Seek to spine LUT item, read from LUT and get out data
  bookFile.seek(lutOffset + sizeof(uint32_t) * index);
  uint32_t spineEntryPos;
  serialization::readPod(bookFile, spineEntryPos);
  bookFile.seek(spineEntryPos);
  return readSpineEntry(bookFile);
}

BookMetadataCache::TocEntry BookMetadataCache::getTocEntry(const int index) {
  if (!loaded) {
    Serial.printf("[%lu] [BMC] getTocEntry called but cache not loaded\n", millis());
    return {};
  }

  if (index < 0 || index >= static_cast<int>(tocCount)) {
    Serial.printf("[%lu] [BMC] getTocEntry index %d out of range\n", millis(), index);
    return {};
  }

  // Seek to TOC LUT item, read from LUT and get out data
  bookFile.seek(lutOffset + sizeof(uint32_t) * spineCount + sizeof(uint32_t) * index);
  uint32_t tocEntryPos;
  serialization::readPod(bookFile, tocEntryPos);
  bookFile.seek(tocEntryPos);
  return readTocEntry(bookFile);
}

BookMetadataCache::SpineEntry BookMetadataCache::readSpineEntry(FsFile& file) const {
  SpineEntry entry;
  serialization::readString(file, entry.href);
  serialization::readPod(file, entry.cumulativeSize);
  serialization::readPod(file, entry.tocIndex);
  return entry;
}

BookMetadataCache::TocEntry BookMetadataCache::readTocEntry(FsFile& file) const {
  TocEntry entry;
  serialization::readString(file, entry.title);
  serialization::readString(file, entry.href);
  serialization::readString(file, entry.anchor);
  serialization::readPod(file, entry.level);
  serialization::readPod(file, entry.spineIndex);
  return entry;
}
