#include "BookMetadataCache.h"

#include <BufferedFile.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>
#include <ZipFile.h>

#include <deque>
#include <optional>

#include "FsHelpers.h"

namespace {
constexpr uint8_t BOOK_CACHE_VERSION = 10;  // v10: ignore ambiguous guide text references
constexpr char bookBinFile[] = "/book.bin";
constexpr char tmpSpineBinFile[] = "/spine.bin.tmp";
constexpr char tmpTocBinFile[] = "/toc.bin.tmp";
// Buffer size for the buildBookBin streams. 3 buffers x 4KB, transient (freed on
// return); 4KB = 8 SD sectors per transfer, enough to stop the sector-cache thrash.
constexpr size_t BUILD_IO_BUFFER_SIZE = 4096;
constexpr size_t MAX_CACHE_STRING_BYTES = 4096;

// Entry (de)serializers, templated so they run over HalFile and the Buffered*
// wrappers alike (two instantiations each -- a few hundred bytes of flash, in
// exchange for the build path streaming at SD speed instead of per-pod).
template <typename F>
uint32_t writeSpineEntryTo(F& file, const BookMetadataCache::SpineEntry& entry) {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.href);
  serialization::writePod(file, entry.cumulativeSize);
  serialization::writePod(file, entry.tocIndex);
  return pos;
}

template <typename F>
uint32_t writeTocEntryTo(F& file, const BookMetadataCache::TocEntry& entry) {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.title);
  serialization::writeString(file, entry.href);
  serialization::writeString(file, entry.anchor);
  serialization::writePod(file, entry.level);
  serialization::writePod(file, entry.spineIndex);
  return pos;
}

template <typename F>
bool readSpineEntryFrom(F& file, BookMetadataCache::SpineEntry& entry) {
  return serialization::tryReadString(file, entry.href, MAX_CACHE_STRING_BYTES) &&
         serialization::tryReadPod(file, entry.cumulativeSize) && serialization::tryReadPod(file, entry.tocIndex);
}

template <typename F>
bool readTocEntryFrom(F& file, BookMetadataCache::TocEntry& entry) {
  return serialization::tryReadString(file, entry.title, MAX_CACHE_STRING_BYTES) &&
         serialization::tryReadString(file, entry.href, MAX_CACHE_STRING_BYTES) &&
         serialization::tryReadString(file, entry.anchor, MAX_CACHE_STRING_BYTES) &&
         serialization::tryReadPod(file, entry.level) && serialization::tryReadPod(file, entry.spineIndex);
}
}  // namespace

/* ============= WRITING / BUILDING FUNCTIONS ================ */

bool BookMetadataCache::beginWrite() {
  buildMode = true;
  buildFailed = false;
  spineCount = 0;
  tocCount = 0;
  LOG_DBG("BMC", "Entering write mode");
  return true;
}

bool BookMetadataCache::beginContentOpfPass() {
  LOG_DBG("BMC", "Beginning content opf pass");

  // Open spine file for writing
  if (!Storage.openFileForWrite("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  // Wrapper OOM is fine: createSpineEntry falls back to unbuffered writes.
  passOut = makeUniqueNoThrow<serialization::BufferedFileWriter>(spineFile, BUILD_IO_BUFFER_SIZE);
  return true;
}

bool BookMetadataCache::endContentOpfPass() {
  const bool flushed = !passOut || passOut->flush();
  passOut.reset();
  // Explicit close() required: member variable persists beyond function scope
  const bool closed = spineFile.close();
  if (!flushed || !closed || buildFailed) {
    LOG_ERR("BMC", "Failed writing spine tmp file");
  }
  return flushed && closed && !buildFailed;
}

bool BookMetadataCache::beginTocPass() {
  LOG_DBG("BMC", "Beginning toc pass");

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  if (!Storage.openFileForWrite("BMC", cachePath + tmpTocBinFile, tocFile)) {
    // Explicit close() required: member variable persists beyond function scope
    spineFile.close();
    return false;
  }

  if (spineCount >= LARGE_SPINE_THRESHOLD) {
    spineHrefIndex.clear();
    spineHrefIndex.resize(spineCount);
    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      SpineEntry entry;
      if (!readSpineEntry(spineFile, entry)) {
        buildFailed = true;
        endTocPass();
        return false;
      }
      SpineHrefIndexEntry idx;
      idx.hrefHash = fnvHash64(entry.href);
      idx.hrefLen = static_cast<uint16_t>(entry.href.size());
      idx.spineIndex = static_cast<int16_t>(i);
      spineHrefIndex[i] = idx;
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

  // Wrapper OOM is fine: createTocEntry falls back to unbuffered writes.
  passOut = makeUniqueNoThrow<serialization::BufferedFileWriter>(tocFile, BUILD_IO_BUFFER_SIZE);
  return true;
}

bool BookMetadataCache::endTocPass() {
  const bool flushed = !passOut || passOut->flush();
  passOut.reset();
  if (!flushed) {
    LOG_ERR("BMC", "Failed writing toc tmp file");
  }
  // Explicit close() required: member variables persist beyond function scope
  const bool tocClosed = tocFile.close();
  const bool spineClosed = spineFile.close();

  spineHrefIndex.clear();
  spineHrefIndex.shrink_to_fit();
  useSpineHrefIndex = false;

  return flushed && tocClosed && spineClosed && !buildFailed;
}

bool BookMetadataCache::endWrite() {
  if (!buildMode) {
    LOG_DBG("BMC", "endWrite called but not in build mode");
    return false;
  }

  buildMode = false;
  if (buildFailed) return false;
  LOG_DBG("BMC", "Wrote %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

bool BookMetadataCache::buildBookBin(const std::string& epubPath, const BookMetadata& metadata) {
  // Open all three files, writing to meta, reading from spine and toc
  if (!Storage.openFileForWrite("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    // Explicit close() required: member variable persists beyond function scope
    bookFile.close();
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpTocBinFile, tocFile)) {
    // Explicit close() required: member variables persist beyond function scope
    bookFile.close();
    spineFile.close();
    return false;
  }

  // Buffered streams for the whole build: every access below is sequential per
  // file, but interleaved ACROSS files, which thrashes SdFat's single shared
  // sector cache when unbuffered (one 512B SD transaction per 4-byte pod --
  // measured 31s for a 1,732-spine omnibus). Three 4KB buffers, freed on return.
  serialization::BufferedFileWriter bookOut(bookFile, BUILD_IO_BUFFER_SIZE);
  serialization::BufferedFileReader spineIn(spineFile, BUILD_IO_BUFFER_SIZE);
  serialization::BufferedFileReader tocIn(tocFile, BUILD_IO_BUFFER_SIZE);
  const auto failRead = [&] {
    LOG_ERR("BMC", "Invalid temporary metadata cache");
    bookOut.flush();
    bookFile.close();
    spineFile.close();
    tocFile.close();
    Storage.remove((cachePath + bookBinFile).c_str());
    return false;
  };

  constexpr uint32_t headerASize =
      sizeof(BOOK_CACHE_VERSION) + /* LUT Offset */ sizeof(uint32_t) + sizeof(spineCount) + sizeof(tocCount);
  const uint32_t metadataSize = metadata.title.size() + metadata.author.size() + metadata.language.size() +
                                metadata.coverItemHref.size() + metadata.textReferenceHref.size() +
                                sizeof(uint32_t) * 5;
  const uint32_t lutSize = sizeof(uint32_t) * spineCount + sizeof(uint32_t) * tocCount;
  const uint32_t lutOffset = headerASize + metadataSize;

  // Header A
  serialization::writePod(bookOut, BOOK_CACHE_VERSION);
  serialization::writePod(bookOut, lutOffset);
  serialization::writePod(bookOut, spineCount);
  serialization::writePod(bookOut, tocCount);
  // Metadata
  serialization::writeString(bookOut, metadata.title);
  serialization::writeString(bookOut, metadata.author);
  serialization::writeString(bookOut, metadata.language);
  serialization::writeString(bookOut, metadata.coverItemHref);
  serialization::writeString(bookOut, metadata.textReferenceHref);

  // Loop through spine entries, writing LUT positions
  spineIn.seek(0);
  for (int i = 0; i < spineCount; i++) {
    const uint32_t pos = spineIn.position();
    SpineEntry entry;
    if (!readSpineEntryFrom(spineIn, entry)) return failRead();
    serialization::writePod(bookOut, pos + lutOffset + lutSize);
  }
  // Total size of the spine tmp file: entries land in book.bin after the toc LUT
  // and the full spine block, so toc LUT positions are offset by it.
  const auto spineBytes = static_cast<uint32_t>(spineIn.position());

  // Loop through toc entries, writing LUT positions
  tocIn.seek(0);
  for (int i = 0; i < tocCount; i++) {
    const uint32_t pos = tocIn.position();
    TocEntry entry;
    if (!readTocEntryFrom(tocIn, entry)) return failRead();
    serialization::writePod(bookOut, pos + lutOffset + lutSize + spineBytes);
  }

  // LUTs complete
  // Loop through spines from spine file matching up TOC indexes, calculating cumulative size and writing to book.bin

  // Build spineIndex->tocIndex mapping in one pass (O(n) instead of O(n*m))
  std::deque<int16_t> spineToTocIndex(spineCount, -1);
  tocIn.seek(0);
  for (int j = 0; j < tocCount; j++) {
    TocEntry tocEntry;
    if (!readTocEntryFrom(tocIn, tocEntry)) return failRead();
    if (tocEntry.spineIndex >= 0 && tocEntry.spineIndex < spineCount) {
      if (spineToTocIndex[tocEntry.spineIndex] == -1) {
        spineToTocIndex[tocEntry.spineIndex] = static_cast<int16_t>(j);
      }
    }
  }

  const bool isZip = !FsHelpers::hasTxtExtension(epubPath) && !FsHelpers::hasMarkdownExtension(epubPath);
  ZipFile zip(epubPath);
  std::optional<std::deque<uint32_t>> spineSizes;
  size_t rawSize = 0;
  if (isZip) {
    // Pre-open zip file to speed up size calculations
    if (!zip.open()) {
      LOG_ERR("BMC", "Could not open EPUB zip for size calculations");
      // Explicit close() required: member variables persist beyond function scope
      bookFile.close();
      spineFile.close();
      tocFile.close();
      return false;
    }
    // NOTE: We intentionally skip calling loadAllFileStatSlims() here.
    // For large EPUBs (2000+ chapters), pre-loading all ZIP central directory entries
    // into memory causes OOM crashes on ESP32-C3's limited ~380KB RAM.
    // Instead, for large books we use a batch lookup that scans the ZIP central
    // directory once per chunk and matches against spine targets using hash comparison.
    // This is O(n*log(m)) instead of O(n*m) while avoiding memory exhaustion.
    // See: https://github.com/crosspoint-reader/crosspoint-reader/issues/134

    if (spineCount >= LARGE_SPINE_THRESHOLD) {
      LOG_DBG("BMC", "Using batch size lookup for %d spine items", spineCount);

      std::deque<ZipFile::SizeTarget> targets;
      spineSizes.emplace(spineCount, 0);
      int matched = 0;

      spineIn.seek(0);
      for (int i = 0; i < spineCount; i++) {
        SpineEntry entry;
        if (!readSpineEntryFrom(spineIn, entry)) return failRead();
        std::string path = FsHelpers::normalisePath(entry.href);

        ZipFile::SizeTarget t;
        t.hash = ZipFile::fnvHash64(path.c_str(), path.size());
        t.len = static_cast<uint16_t>(path.size());
        t.index = static_cast<uint16_t>(i);
        targets.push_back(t);
        // Targets take 16 B per spine item, so look sizes up 2,048 items at a time.
        if (targets.size() < 2048 && i + 1 < spineCount) continue;

        std::sort(targets.begin(), targets.end(), [](const ZipFile::SizeTarget& a, const ZipFile::SizeTarget& b) {
          return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
        });
        matched += zip.fillUncompressedSizes(targets, *spineSizes);
        targets.clear();
      }
      LOG_DBG("BMC", "Batch lookup matched %d/%d spine items", matched, spineCount);
    }
  } else {
    HalFile rawFile;
    if (Storage.openFileForRead("BMC", epubPath, rawFile)) {
      rawSize = rawFile.size();
    }
  }

  uint32_t cumSize = 0;
  spineIn.seek(0);
  int lastSpineTocIndex = -1;
  for (int i = 0; i < spineCount; i++) {
    SpineEntry spineEntry;
    if (!readSpineEntryFrom(spineIn, spineEntry)) return failRead();
    spineEntry.tocIndex = spineToTocIndex[i];
    if (spineEntry.tocIndex == -1) {
      if (isZip) {
        LOG_DBG("BMC", "Warning: Could not find TOC entry for spine item %d: %s, using title from last section", i,
                spineEntry.href.c_str());
      }
      spineEntry.tocIndex = lastSpineTocIndex;
    }
    lastSpineTocIndex = spineEntry.tocIndex;

    size_t itemSize = rawSize;
    if (isZip) {
      itemSize = spineSizes ? (*spineSizes)[i] : 0;
      if (itemSize == 0) {
        const std::string path = FsHelpers::normalisePath(spineEntry.href);
        if (!zip.getInflatedFileSize(path.c_str(), &itemSize)) {
          LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
        }
      }
    }

    cumSize += itemSize;
    spineEntry.cumulativeSize = cumSize;
    writeSpineEntryTo(bookOut, spineEntry);
  }
  spineSizes.reset();
  if (isZip) zip.close();

  // Loop through toc entries from toc file writing to book.bin
  tocIn.seek(0);
  for (int i = 0; i < tocCount; i++) {
    TocEntry tocEntry;
    if (!readTocEntryFrom(tocIn, tocEntry)) return failRead();
    writeTocEntryTo(bookOut, tocEntry);
  }

  const bool written = bookOut.flush();

  // Explicit close() required: member variables persist beyond function scope
  bookFile.close();
  spineFile.close();
  tocFile.close();

  if (!written) {
    // A short write (card full/removed) would leave a truncated book.bin that
    // still passes the version check on load; remove it so the next open rebuilds.
    LOG_ERR("BMC", "Failed writing book.bin, removing truncated file");
    Storage.remove((cachePath + bookBinFile).c_str());
    return false;
  }

  LOG_DBG("BMC", "Successfully built book.bin");
  return true;
}

bool BookMetadataCache::cleanupTmpFiles() const {
  const auto spineBinFile = cachePath + tmpSpineBinFile;
  if (Storage.exists(spineBinFile.c_str())) {
    Storage.remove(spineBinFile.c_str());
  }
  const auto tocBinFile = cachePath + tmpTocBinFile;
  if (Storage.exists(tocBinFile.c_str())) {
    Storage.remove(tocBinFile.c_str());
  }
  return true;
}

uint32_t BookMetadataCache::writeSpineEntry(HalFile& file, const SpineEntry& entry) const {
  return writeSpineEntryTo(file, entry);
}

uint32_t BookMetadataCache::writeTocEntry(HalFile& file, const TocEntry& entry) const {
  return writeTocEntryTo(file, entry);
}

// Note: for the LUT to be accurate, this **MUST** be called for all spine items before `addTocEntry` is ever called
// this is because in this function we're marking positions of the items
void BookMetadataCache::createSpineEntry(const std::string& href) {
  if (!buildMode || !spineFile) {
    LOG_DBG("BMC", "createSpineEntry called but not in build mode");
    return;
  }

  const SpineEntry entry(href, 0, -1);
  if (passOut) {
    writeSpineEntryTo(*passOut, entry);
  } else {
    writeSpineEntry(spineFile, entry);
  }
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
      SpineEntry spineEntry;
      if (!readSpineEntry(spineFile, spineEntry)) {
        buildFailed = true;
        return;
      }
      if (spineEntry.href == href) {
        spineIndex = static_cast<int16_t>(i);
        break;
      }
    }
    if (spineIndex == -1) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  }

  // Compose the title to NFC at index time so the cache stores precomposed glyphs;
  // device fonts have no combining-mark positioning, so NFD titles render broken.
  const TocEntry entry(utf8ComposeNfc(title), href, anchor, level, spineIndex);
  if (passOut) {
    writeTocEntryTo(*passOut, entry);
  } else {
    writeTocEntry(tocFile, entry);
  }
  tocCount++;
}

/* ============= READING / LOADING FUNCTIONS ================ */

bool BookMetadataCache::load() {
  loaded = false;
  spineCount = tocCount = 0;
  cumulativeSizes.reset();
  coreMetadata = {};
  if (!Storage.openFileForRead("BMC", cachePath + bookBinFile, bookFile)) return false;
  const auto fail = [this] {
    LOG_ERR("BMC", "Invalid metadata cache");
    bookFile.close();
    cumulativeSizes.reset();
    spineCount = tocCount = 0;
    coreMetadata = {};
    return false;
  };
  uint8_t version = 0;
  if (!serialization::tryReadPod(bookFile, version) || version != BOOK_CACHE_VERSION ||
      !serialization::tryReadPod(bookFile, lutOffset) || !serialization::tryReadPod(bookFile, spineCount) ||
      !serialization::tryReadPod(bookFile, tocCount) ||
      !serialization::tryReadString(bookFile, coreMetadata.title, MAX_CACHE_STRING_BYTES) ||
      !serialization::tryReadString(bookFile, coreMetadata.author, MAX_CACHE_STRING_BYTES) ||
      !serialization::tryReadString(bookFile, coreMetadata.language, MAX_CACHE_STRING_BYTES) ||
      !serialization::tryReadString(bookFile, coreMetadata.coverItemHref, MAX_CACHE_STRING_BYTES) ||
      !serialization::tryReadString(bookFile, coreMetadata.textReferenceHref, MAX_CACHE_STRING_BYTES))
    return fail();
  const size_t size = bookFile.size();
  const size_t lutSize = (static_cast<size_t>(spineCount) + tocCount) * sizeof(uint32_t);
  const size_t minimumEntries = static_cast<size_t>(spineCount) * 10 + static_cast<size_t>(tocCount) * 15;
  if (bookFile.position() != lutOffset || lutOffset > size || lutSize > size - lutOffset ||
      minimumEntries > size - lutOffset - lutSize)
    return fail();
  cumulativeSizes = makeUniqueNoThrow<uint32_t[]>(spineCount);
  if (spineCount && !cumulativeSizes) return fail();
  HalFile lookupFile;
  if (!Storage.openFileForRead("BMC", cachePath + bookBinFile, lookupFile) || !lookupFile.seek(lutOffset) ||
      !bookFile.seek(lutOffset + lutSize))
    return fail();
  // Separate buffered cursors validate lookup positions without a seek per entry.
  serialization::BufferedFileReader lookups(lookupFile, 1024);
  serialization::BufferedFileReader entries(bookFile, 1024);
  uint32_t previousSize = 0;
  for (uint32_t i = 0; i < static_cast<uint32_t>(spineCount) + tocCount; ++i) {
    uint32_t position = 0;
    if (!serialization::tryReadPod(lookups, position) || position != entries.position()) return fail();
    if (i < spineCount) {
      SpineEntry entry;
      if (!readSpineEntryFrom(entries, entry) || entry.cumulativeSize < previousSize || entry.tocIndex < -1 ||
          entry.tocIndex >= static_cast<int>(tocCount))
        return fail();
      previousSize = cumulativeSizes[i] = entry.cumulativeSize;
    } else {
      TocEntry entry;
      if (!readTocEntryFrom(entries, entry) || entry.spineIndex < -1 ||
          entry.spineIndex >= static_cast<int>(spineCount))
        return fail();
    }
  }
  loaded = true;
  LOG_DBG("BMC", "Loaded cache data: %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

uint32_t BookMetadataCache::getCumulativeSize(const int index) const {
  if (index < 0 || !loaded || !cumulativeSizes || index >= static_cast<int>(spineCount)) {
    return 0;
  }
  return cumulativeSizes[index];
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

  uint32_t position = 0;
  SpineEntry entry;
  if (!bookFile.seek(lutOffset + sizeof(uint32_t) * index) || !serialization::tryReadPod(bookFile, position) ||
      !bookFile.seek(position) || !readSpineEntry(bookFile, entry)) {
    loaded = false;
    LOG_ERR("BMC", "Failed reading spine entry %d", index);
    return {};
  }
  return entry;
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

  uint32_t position = 0;
  TocEntry entry;
  if (!bookFile.seek(lutOffset + sizeof(uint32_t) * (spineCount + index)) ||
      !serialization::tryReadPod(bookFile, position) || !bookFile.seek(position) || !readTocEntry(bookFile, entry)) {
    loaded = false;
    LOG_ERR("BMC", "Failed reading TOC entry %d", index);
    return {};
  }
  return entry;
}

bool BookMetadataCache::readSpineEntry(HalFile& file, SpineEntry& entry) const {
  return readSpineEntryFrom(file, entry);
}

bool BookMetadataCache::readTocEntry(HalFile& file, TocEntry& entry) const { return readTocEntryFrom(file, entry); }
