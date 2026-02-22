#include "BookMetadataCache.h"

#include <Logging.h>
#include <Serialization.h>
#include <ZipFile.h>

#include <vector>

#include "FsHelpers.h"

namespace {
constexpr uint8_t BOOK_CACHE_VERSION = 5;
constexpr char bookBinFile[] = "/book.bin";
constexpr char tmpSpineBinFile[] = "/spine.bin.tmp";
constexpr char tmpTocBinFile[] = "/toc.bin.tmp";
}  // namespace

/* ============= WRITING / BUILDING FUNCTIONS ================ */

bool BookMetadataCache::beginWrite() {
  buildMode = true;
  spineCount = 0;
  tocCount = 0;
  LOG_DBG("BMC", "Entering write mode");
  return true;
}

bool BookMetadataCache::beginContentOpfPass() {
  LOG_DBG("BMC", "Beginning content opf pass");

  // Open spine file for writing
  return Storage.openFileForWrite("BMC", cachePath + tmpSpineBinFile, spineFile);
}

bool BookMetadataCache::endContentOpfPass() {
  spineFile.close();
  return true;
}

bool BookMetadataCache::beginTocPass() {
  LOG_DBG("BMC", "Beginning toc pass");

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  if (!Storage.openFileForWrite("BMC", cachePath + tmpTocBinFile, tocFile)) {
    spineFile.close();
    return false;
  }

  if (spineCount >= LARGE_SPINE_THRESHOLD) {
    spineHrefIndex.clear();
    spineHrefIndex.reserve(spineCount);
    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto entry = readSpineEntry(spineFile);
      SpineHrefIndexEntry idx;
      idx.hrefHash = fnvHash64(entry.href);
      idx.hrefLen = static_cast<uint16_t>(entry.href.size());
      idx.spineIndex = static_cast<int16_t>(i);
      spineHrefIndex.push_back(idx);
    }
    std::sort(spineHrefIndex.begin(), spineHrefIndex.end(),
              [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
              });
    spineFile.seek(0);
    useSpineHrefIndex = true;
    LOG_DBG("BMC", "Using fast index for %d spine items", spineCount);
  } else {
    useSpineHrefIndex = false;
  }

  return true;
}

bool BookMetadataCache::endTocPass() {
  tocFile.close();
  spineFile.close();

  spineHrefIndex.clear();
  spineHrefIndex.shrink_to_fit();
  useSpineHrefIndex = false;

  return true;
}

bool BookMetadataCache::endWrite() {
  if (!buildMode) {
    LOG_DBG("BMC", "endWrite called but not in build mode");
    return false;
  }

  buildMode = false;
  LOG_DBG("BMC", "Wrote %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

bool BookMetadataCache::buildBookBin(const std::string& epubPath, const BookMetadata& metadata) {
  // Open all three files, writing to meta, reading from spine and toc
  if (!Storage.openFileForWrite("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    bookFile.close();
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpTocBinFile, tocFile)) {
    bookFile.close();
    spineFile.close();
    return false;
  }

  constexpr uint32_t headerASize =
      sizeof(BOOK_CACHE_VERSION) + /* LUT Offset */ sizeof(uint32_t) + sizeof(spineCount) + sizeof(tocCount);
  const uint32_t metadataSize = metadata.title.size() + metadata.author.size() + metadata.language.size() +
                                metadata.coverItemHref.size() + metadata.textReferenceHref.size() +
                                sizeof(uint32_t) * 5;
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
  serialization::writeString(bookFile, metadata.language);
  serialization::writeString(bookFile, metadata.coverItemHref);
  serialization::writeString(bookFile, metadata.textReferenceHref);

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

  // Build spineIndex->tocIndex mapping in one pass (O(n) instead of O(n*m))
  std::vector<int16_t> spineToTocIndex(spineCount, -1);
  tocFile.seek(0);
  for (int j = 0; j < tocCount; j++) {
    auto tocEntry = readTocEntry(tocFile);
    if (tocEntry.spineIndex >= 0 && tocEntry.spineIndex < spineCount) {
      if (spineToTocIndex[tocEntry.spineIndex] == -1) {
        spineToTocIndex[tocEntry.spineIndex] = static_cast<int16_t>(j);
      }
    }
  }

  ZipFile zip(epubPath);
  // Pre-open zip file to speed up size calculations
  if (!zip.open()) {
    LOG_ERR("BMC", "Could not open EPUB zip for size calculations");
    bookFile.close();
    spineFile.close();
    tocFile.close();
    return false;
  }
  // NOTE: We intentionally skip calling loadAllFileStatSlims() here.
  // For large EPUBs (2000+ chapters), pre-loading all ZIP central directory entries
  // into memory causes OOM crashes on ESP32-C3's limited ~380KB RAM.
  // Instead, for large books we use a one-pass batch lookup that scans the ZIP
  // central directory once and matches against spine targets using hash comparison.
  // This is O(n*log(m)) instead of O(n*m) while avoiding memory exhaustion.
  // See: https://github.com/crosspoint-reader/crosspoint-reader/issues/134

  std::vector<uint32_t> spineSizes;
  bool useBatchSizes = false;

  if (spineCount >= LARGE_SPINE_THRESHOLD) {
    LOG_DBG("BMC", "Using batch size lookup for %d spine items", spineCount);

    std::vector<ZipFile::SizeTarget> targets;
    targets.reserve(spineCount);

    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto entry = readSpineEntry(spineFile);
      std::string path = FsHelpers::normalisePath(entry.href);

      ZipFile::SizeTarget t;
      t.hash = ZipFile::fnvHash64(path.c_str(), path.size());
      t.len = static_cast<uint16_t>(path.size());
      t.index = static_cast<uint16_t>(i);
      targets.push_back(t);
    }

    std::sort(targets.begin(), targets.end(), [](const ZipFile::SizeTarget& a, const ZipFile::SizeTarget& b) {
      return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
    });

    spineSizes.resize(spineCount, 0);
    int matched = zip.fillUncompressedSizes(targets, spineSizes);
    LOG_DBG("BMC", "Batch lookup matched %d/%d spine items", matched, spineCount);

    targets.clear();
    targets.shrink_to_fit();

    useBatchSizes = true;
  }

  uint32_t cumSize = 0;
  spineFile.seek(0);
  int lastSpineTocIndex = -1;
  for (int i = 0; i < spineCount; i++) {
    auto spineEntry = readSpineEntry(spineFile);

    spineEntry.tocIndex = spineToTocIndex[i];

    // Not a huge deal if we don't fine a TOC entry for the spine entry, this is expected behaviour for EPUBs
    // Logging here is for debugging
    if (spineEntry.tocIndex == -1) {
      LOG_DBG("BMC", "Warning: Could not find TOC entry for spine item %d: %s, using title from last section", i,
              spineEntry.href.c_str());
      spineEntry.tocIndex = lastSpineTocIndex;
    }
    lastSpineTocIndex = spineEntry.tocIndex;

    size_t itemSize = 0;
    if (useBatchSizes) {
      itemSize = spineSizes[i];
      if (itemSize == 0) {
        const std::string path = FsHelpers::normalisePath(spineEntry.href);
        if (!zip.getInflatedFileSize(path.c_str(), &itemSize)) {
          LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
        }
      }
    } else {
      const std::string path = FsHelpers::normalisePath(spineEntry.href);
      if (!zip.getInflatedFileSize(path.c_str(), &itemSize)) {
        LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
      }
    }

    cumSize += itemSize;
    spineEntry.cumulativeSize = cumSize;

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

  LOG_DBG("BMC", "Successfully built book.bin");
  return true;
}

bool BookMetadataCache::cleanupTmpFiles() const {
  if (Storage.exists((cachePath + tmpSpineBinFile).c_str())) {
    Storage.remove((cachePath + tmpSpineBinFile).c_str());
  }
  if (Storage.exists((cachePath + tmpTocBinFile).c_str())) {
    Storage.remove((cachePath + tmpTocBinFile).c_str());
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
    LOG_DBG("BMC", "createSpineEntry called but not in build mode");
    return;
  }

  const SpineEntry entry(href, 0, -1);
  writeSpineEntry(spineFile, entry);
  spineCount++;
}

void BookMetadataCache::createTocEntry(const std::string& title, const std::string& href, const std::string& anchor,
                                       const uint8_t level) {
  if (!buildMode || !tocFile || !spineFile) {
    LOG_DBG("BMC", "createTocEntry called but not in build mode");
    return;
  }

  int16_t spineIndex = -1;

  if (useSpineHrefIndex) {
    uint64_t targetHash = fnvHash64(href);
    uint16_t targetLen = static_cast<uint16_t>(href.size());

    auto it =
        std::lower_bound(spineHrefIndex.begin(), spineHrefIndex.end(), SpineHrefIndexEntry{targetHash, targetLen, 0},
                         [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                           return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
                         });

    while (it != spineHrefIndex.end() && it->hrefHash == targetHash && it->hrefLen == targetLen) {
      spineIndex = it->spineIndex;
      break;
    }

    if (spineIndex == -1) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  } else {
    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto spineEntry = readSpineEntry(spineFile);
      if (spineEntry.href == href) {
        spineIndex = static_cast<int16_t>(i);
        break;
      }
    }
    if (spineIndex == -1) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  }

  const TocEntry entry(title, href, anchor, level, spineIndex);
  writeTocEntry(tocFile, entry);
  tocCount++;
}

/* ============= READING / LOADING FUNCTIONS ================ */

bool BookMetadataCache::load() {
  if (!Storage.openFileForRead("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(bookFile, version);
  if (version != BOOK_CACHE_VERSION) {
    LOG_DBG("BMC", "Cache version mismatch: expected %d, got %d", BOOK_CACHE_VERSION, version);
    bookFile.close();
    return false;
  }

  serialization::readPod(bookFile, lutOffset);
  serialization::readPod(bookFile, spineCount);
  serialization::readPod(bookFile, tocCount);

  serialization::readString(bookFile, coreMetadata.title);
  serialization::readString(bookFile, coreMetadata.author);
  serialization::readString(bookFile, coreMetadata.language);
  serialization::readString(bookFile, coreMetadata.coverItemHref);
  serialization::readString(bookFile, coreMetadata.textReferenceHref);

  loaded = true;
  LOG_DBG("BMC", "Loaded cache data: %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

BookMetadataCache::SpineEntry BookMetadataCache::getSpineEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getSpineEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(spineCount)) {
    LOG_ERR("BMC", "getSpineEntry index %d out of range", index);
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
    LOG_ERR("BMC", "getTocEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(tocCount)) {
    LOG_ERR("BMC", "getTocEntry index %d out of range", index);
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
