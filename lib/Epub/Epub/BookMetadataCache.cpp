#include "BookMetadataCache.h"

#include <BufferedFile.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>
#include <ZipFile.h>

#include <array>
#include <deque>

#include "FsHelpers.h"
#include "SourceIdentityCodec.h"

namespace {
// v10 binds all derived metadata and section caches to the backing EPUB's ZIP
// central directory, preventing a different book at the same path from
// inheriting them.
constexpr uint8_t BOOK_CACHE_VERSION = 10;
constexpr uint32_t BOOK_CACHE_COMMIT_MARKER = 0x424D434B;  // "BMCK"
constexpr char bookBinFile[] = "/book.bin";
constexpr char tmpSpineBinFile[] = "/spine.bin.tmp";
constexpr char tmpTocBinFile[] = "/toc.bin.tmp";
// Buffer size for the buildBookBin streams. 3 buffers x 4KB, transient (freed on
// return); 4KB = 8 SD sectors per transfer, enough to stop the sector-cache thrash.
constexpr size_t BUILD_IO_BUFFER_SIZE = 4096;
constexpr size_t BOOK_CACHE_FIXED_HEADER_SIZE = sizeof(BOOK_CACHE_VERSION) + sizeof(uint32_t) + sizeof(uint16_t) +
                                                sizeof(uint16_t) + SourceIdentityCodec::PAYLOAD_SIZE + sizeof(uint32_t);
constexpr size_t BOOK_CACHE_MIN_METADATA_SIZE = sizeof(uint32_t) * 5;
constexpr size_t BOOK_CACHE_MAX_METADATA_SIZE = 64 * 1024;
// No valid path/title/anchor entry should consume a material fraction of the
// ESP32-C3 heap. This also prevents a corrupt LUT from turning a bounded read
// into a large std::string allocation.
constexpr size_t BOOK_CACHE_MAX_ENTRY_SIZE = 32 * 1024;
constexpr size_t BOOK_CACHE_MIN_FILE_SIZE =
    BOOK_CACHE_FIXED_HEADER_SIZE + BOOK_CACHE_MIN_METADATA_SIZE + sizeof(BOOK_CACHE_COMMIT_MARKER);

template <typename T>
bool readPodExact(HalFile& file, T& value) {
  return file.read(&value, sizeof(value)) == static_cast<int>(sizeof(value));
}

bool consumeBoundedString(HalFile& file, const size_t endPosition, std::string* value) {
  uint32_t length = 0;
  if (!readPodExact(file, length)) return false;

  const size_t position = file.position();
  if (position > endPosition || length > endPosition - position) return false;

  if (!value) return file.seek(position + length);

  value->resize(length);
  return length == 0 || file.read(value->data(), length) == static_cast<int>(length);
}

bool inspectSpineEntry(HalFile& file, const size_t endPosition, const uint16_t tocCount,
                       uint32_t* cumulativeSize = nullptr) {
  if (!consumeBoundedString(file, endPosition, nullptr)) return false;
  uint32_t cumulative = 0;
  int16_t tocIndex = -1;
  if (!readPodExact(file, cumulative) || !readPodExact(file, tocIndex) || file.position() != endPosition ||
      tocIndex < -1 || tocIndex >= static_cast<int32_t>(tocCount)) {
    return false;
  }
  if (cumulativeSize) *cumulativeSize = cumulative;
  return true;
}

bool inspectTocEntry(HalFile& file, const size_t endPosition, const uint16_t spineCount) {
  if (!consumeBoundedString(file, endPosition, nullptr) || !consumeBoundedString(file, endPosition, nullptr) ||
      !consumeBoundedString(file, endPosition, nullptr)) {
    return false;
  }
  uint8_t level = 0;
  int16_t spineIndex = -1;
  return readPodExact(file, level) && readPodExact(file, spineIndex) && file.position() == endPosition &&
         spineIndex >= -1 && spineIndex < static_cast<int32_t>(spineCount);
}

bool readSpineEntryChecked(HalFile& file, const size_t endPosition, const uint16_t tocCount,
                           BookMetadataCache::SpineEntry& entry) {
  BookMetadataCache::SpineEntry parsed;
  if (!consumeBoundedString(file, endPosition, &parsed.href) || !readPodExact(file, parsed.cumulativeSize) ||
      !readPodExact(file, parsed.tocIndex) || file.position() != endPosition || parsed.tocIndex < -1 ||
      parsed.tocIndex >= static_cast<int32_t>(tocCount)) {
    return false;
  }
  entry = std::move(parsed);
  return true;
}

bool readTocEntryChecked(HalFile& file, const size_t endPosition, const uint16_t spineCount,
                         BookMetadataCache::TocEntry& entry) {
  BookMetadataCache::TocEntry parsed;
  if (!consumeBoundedString(file, endPosition, &parsed.title) ||
      !consumeBoundedString(file, endPosition, &parsed.href) ||
      !consumeBoundedString(file, endPosition, &parsed.anchor) || !readPodExact(file, parsed.level) ||
      !readPodExact(file, parsed.spineIndex) || file.position() != endPosition || parsed.spineIndex < -1 ||
      parsed.spineIndex >= static_cast<int32_t>(spineCount)) {
    return false;
  }
  entry = std::move(parsed);
  return true;
}

bool validEntryBounds(const uint32_t lutOffset, const uint32_t entryCount, const size_t dataEndOffset,
                      const uint32_t start, const uint32_t end) {
  const uint64_t dataStart = static_cast<uint64_t>(lutOffset) + static_cast<uint64_t>(entryCount) * sizeof(uint32_t);
  return start >= dataStart && end > start && end <= dataEndOffset && end - start <= BOOK_CACHE_MAX_ENTRY_SIZE;
}

bool readEntryBounds(HalFile& file, const uint32_t lutOffset, const uint32_t entryCount, const uint32_t index,
                     const size_t dataEndOffset, size_t& entryStart, size_t& entryEnd) {
  if (index >= entryCount) return false;
  const uint64_t lutPosition = static_cast<uint64_t>(lutOffset) + static_cast<uint64_t>(index) * sizeof(uint32_t);
  if (lutPosition > SIZE_MAX || !file.seek(static_cast<size_t>(lutPosition))) return false;

  uint32_t start = 0;
  if (!readPodExact(file, start)) return false;

  uint32_t end = 0;
  if (index + 1 < entryCount) {
    if (!readPodExact(file, end)) return false;
  } else {
    if (dataEndOffset > UINT32_MAX) return false;
    end = static_cast<uint32_t>(dataEndOffset);
  }

  if (!validEntryBounds(lutOffset, entryCount, dataEndOffset, start, end)) return false;
  entryStart = start;
  entryEnd = end;
  return true;
}

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
BookMetadataCache::SpineEntry readSpineEntryFrom(F& file) {
  BookMetadataCache::SpineEntry entry;
  serialization::readString(file, entry.href);
  serialization::readPod(file, entry.cumulativeSize);
  serialization::readPod(file, entry.tocIndex);
  return entry;
}

template <typename F>
BookMetadataCache::TocEntry readTocEntryFrom(F& file) {
  BookMetadataCache::TocEntry entry;
  serialization::readString(file, entry.title);
  serialization::readString(file, entry.href);
  serialization::readString(file, entry.anchor);
  serialization::readPod(file, entry.level);
  serialization::readPod(file, entry.spineIndex);
  return entry;
}
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
  spineFile.close();
  if (!flushed) {
    LOG_ERR("BMC", "Failed writing spine tmp file");
  }
  return flushed;
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
      auto entry = readSpineEntry(spineFile);
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
  tocFile.close();
  spineFile.close();

  spineHrefIndex.clear();
  spineHrefIndex.shrink_to_fit();
  useSpineHrefIndex = false;

  return flushed;
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

bool BookMetadataCache::buildBookBin(const std::string& epubPath, const BookMetadata& metadata,
                                     const ZipFile::SourceIdentity& sourceIdentity) {
  const uint64_t metadataSize64 = BOOK_CACHE_MIN_METADATA_SIZE + static_cast<uint64_t>(metadata.title.size()) +
                                  metadata.author.size() + metadata.language.size() + metadata.coverItemHref.size() +
                                  metadata.textReferenceHref.size();
  if (metadataSize64 > BOOK_CACHE_MAX_METADATA_SIZE) {
    LOG_ERR("BMC", "Book metadata is too large to cache safely (%llu bytes)",
            static_cast<unsigned long long>(metadataSize64));
    return false;
  }

  SourceIdentityCodec::Payload identityPayload;
  if (!SourceIdentityCodec::encodePayload(sourceIdentity, identityPayload)) return false;
  const uint32_t identityChecksum = SourceIdentityCodec::crc32(identityPayload.data(), identityPayload.size());

  // Refuse to publish metadata parsed from an EPUB that changed during the
  // indexing passes. This check happens before book.bin is truncated.
  ZipFile identityZip(epubPath);
  ZipFile::SourceIdentity currentIdentity;
  if (!identityZip.getSourceIdentity(currentIdentity) || currentIdentity != sourceIdentity) {
    LOG_ERR("BMC", "EPUB changed while metadata was being indexed");
    return false;
  }

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

  constexpr uint32_t headerASize = BOOK_CACHE_FIXED_HEADER_SIZE;
  const uint32_t metadataSize = static_cast<uint32_t>(metadataSize64);
  const uint32_t lutSize = sizeof(uint32_t) * spineCount + sizeof(uint32_t) * tocCount;
  const uint32_t lutOffset = headerASize + metadataSize;

  // Header A
  serialization::writePod(bookOut, BOOK_CACHE_VERSION);
  serialization::writePod(bookOut, lutOffset);
  serialization::writePod(bookOut, spineCount);
  serialization::writePod(bookOut, tocCount);
  bookOut.write(identityPayload.data(), identityPayload.size());
  serialization::writePod(bookOut, identityChecksum);
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
    readSpineEntryFrom(spineIn);
    serialization::writePod(bookOut, pos + lutOffset + lutSize);
  }
  // Total size of the spine tmp file: entries land in book.bin after the toc LUT
  // and the full spine block, so toc LUT positions are offset by it.
  const auto spineBytes = static_cast<uint32_t>(spineIn.position());

  // Loop through toc entries, writing LUT positions
  tocIn.seek(0);
  for (int i = 0; i < tocCount; i++) {
    const uint32_t pos = tocIn.position();
    readTocEntryFrom(tocIn);
    serialization::writePod(bookOut, pos + lutOffset + lutSize + spineBytes);
  }

  // LUTs complete
  // Loop through spines from spine file matching up TOC indexes, calculating cumulative size and writing to book.bin

  // Build spineIndex->tocIndex mapping in one pass (O(n) instead of O(n*m))
  std::deque<int16_t> spineToTocIndex(spineCount, -1);
  tocIn.seek(0);
  for (int j = 0; j < tocCount; j++) {
    auto tocEntry = readTocEntryFrom(tocIn);
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
    // Explicit close() required: member variables persist beyond function scope
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

  std::deque<uint32_t> spineSizes;
  bool useBatchSizes = false;

  if (spineCount >= LARGE_SPINE_THRESHOLD) {
    LOG_DBG("BMC", "Using batch size lookup for %d spine items", spineCount);

    std::deque<ZipFile::SizeTarget> targets;
    targets.resize(spineCount);

    spineIn.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto entry = readSpineEntryFrom(spineIn);
      std::string path = FsHelpers::normalisePath(entry.href);

      ZipFile::SizeTarget t;
      t.hash = ZipFile::fnvHash64(path.c_str(), path.size());
      t.len = static_cast<uint16_t>(path.size());
      t.index = static_cast<uint16_t>(i);
      targets[i] = t;
    }

    std::sort(targets.begin(), targets.end(), [](const ZipFile::SizeTarget& a, const ZipFile::SizeTarget& b) {
      return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
    });

    spineSizes.resize(spineCount, 0);
    int matched = zip.fillUncompressedSizes(targets, spineSizes);
    LOG_DBG("BMC", "Batch lookup matched %d/%d spine items", matched, spineCount);
    (void)matched;

    targets.clear();
    targets.shrink_to_fit();

    useBatchSizes = true;
  }

  uint32_t cumSize = 0;
  spineIn.seek(0);
  int lastSpineTocIndex = -1;
  for (int i = 0; i < spineCount; i++) {
    auto spineEntry = readSpineEntryFrom(spineIn);

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
    writeSpineEntryTo(bookOut, spineEntry);
  }
  // Close opened zip file
  zip.close();

  // Loop through toc entries from toc file writing to book.bin
  tocIn.seek(0);
  for (int i = 0; i < tocCount; i++) {
    auto tocEntry = readTocEntryFrom(tocIn);
    writeTocEntryTo(bookOut, tocEntry);
  }

  // Written last so a torn/short build can never be mistaken for a complete
  // cache on the next boot.
  serialization::writePod(bookOut, BOOK_CACHE_COMMIT_MARKER);
  const bool written = bookOut.flush();

  // Explicit close() required: member variables persist beyond function scope
  const bool closed = bookFile.close();
  spineFile.close();
  tocFile.close();

  if (!written || !closed) {
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

BookMetadataCache::LoadStatus BookMetadataCache::load(const ZipFile::SourceIdentity& expectedSourceIdentity) {
  loaded = false;
  coreMetadata = {};
  spineCount = 0;
  tocCount = 0;
  lutOffset = 0;
  dataEndOffset = 0;
  loadedFileSize = 0;
  lastLoadStatus = LoadStatus::Missing;
  const std::string path = cachePath + bookBinFile;
  if (!Storage.exists(path.c_str())) return lastLoadStatus;
  if (!Storage.openFileForRead("BMC", cachePath + bookBinFile, bookFile)) {
    lastLoadStatus = LoadStatus::IoError;
    return lastLoadStatus;
  }

  const auto fail = [this](const LoadStatus status) {
    bookFile.close();
    loaded = false;
    dataEndOffset = 0;
    loadedFileSize = 0;
    lastLoadStatus = status;
    return status;
  };

  const size_t fileSize = bookFile.size();
  if (fileSize < sizeof(BOOK_CACHE_VERSION)) {
    LOG_DBG("BMC", "Cache file is truncated");
    return fail(LoadStatus::Invalid);
  }

  uint8_t version = 0;
  if (!readPodExact(bookFile, version)) return fail(LoadStatus::Invalid);
  if (version < BOOK_CACHE_VERSION) {
    LOG_DBG("BMC", "Legacy cache version: %d", version);
    return fail(LoadStatus::LegacyVersion);
  }
  if (version > BOOK_CACHE_VERSION) {
    LOG_DBG("BMC", "Newer cache version: %d", version);
    return fail(LoadStatus::NewerVersion);
  }

  if (fileSize < BOOK_CACHE_MIN_FILE_SIZE) {
    LOG_DBG("BMC", "Cache file is truncated");
    return fail(LoadStatus::Invalid);
  }

  uint32_t commitMarker = 0;
  if (!bookFile.seek(fileSize - sizeof(commitMarker)) || !readPodExact(bookFile, commitMarker) ||
      commitMarker != BOOK_CACHE_COMMIT_MARKER || !bookFile.seek(sizeof(version))) {
    LOG_DBG("BMC", "Cache commit marker is missing");
    return fail(LoadStatus::Invalid);
  }

  if (!readPodExact(bookFile, lutOffset) || !readPodExact(bookFile, spineCount) || !readPodExact(bookFile, tocCount)) {
    return fail(LoadStatus::Invalid);
  }

  SourceIdentityCodec::Payload identityPayload{};
  uint32_t identityChecksum = 0;
  if (bookFile.read(identityPayload.data(), identityPayload.size()) != static_cast<int>(identityPayload.size()) ||
      !readPodExact(bookFile, identityChecksum)) {
    return fail(LoadStatus::Invalid);
  }
  ZipFile::SourceIdentity storedIdentity;
  if (identityChecksum != SourceIdentityCodec::crc32(identityPayload.data(), identityPayload.size()) ||
      !SourceIdentityCodec::decodePayload(identityPayload.data(), identityPayload.size(), storedIdentity)) {
    LOG_ERR("BMC", "Cache source identity is corrupt");
    return fail(LoadStatus::Invalid);
  }

  if (storedIdentity != expectedSourceIdentity) {
    LOG_ERR("BMC", "Backing EPUB no longer matches book cache");
    return fail(LoadStatus::SourceMismatch);
  }

  const size_t minimumLutOffset = BOOK_CACHE_FIXED_HEADER_SIZE + BOOK_CACHE_MIN_METADATA_SIZE;
  if (lutOffset < minimumLutOffset || lutOffset - BOOK_CACHE_FIXED_HEADER_SIZE > BOOK_CACHE_MAX_METADATA_SIZE) {
    LOG_DBG("BMC", "Cache metadata bounds are invalid");
    return fail(LoadStatus::Invalid);
  }

  const uint64_t lutSize = static_cast<uint64_t>(spineCount + tocCount) * sizeof(uint32_t);
  const uint64_t dataEnd = fileSize - sizeof(BOOK_CACHE_COMMIT_MARKER);
  const uint64_t dataStart = static_cast<uint64_t>(lutOffset) + lutSize;
  if (dataStart > dataEnd || dataEnd > UINT32_MAX) {
    LOG_DBG("BMC", "Cache LUT bounds are invalid");
    return fail(LoadStatus::Invalid);
  }

  // Validate without allocating first. A corrupt length must never drive a
  // large std::string::resize on this memory-constrained target.
  if (!bookFile.seek(BOOK_CACHE_FIXED_HEADER_SIZE) || !consumeBoundedString(bookFile, lutOffset, nullptr) ||
      !consumeBoundedString(bookFile, lutOffset, nullptr) || !consumeBoundedString(bookFile, lutOffset, nullptr) ||
      !consumeBoundedString(bookFile, lutOffset, nullptr) || !consumeBoundedString(bookFile, lutOffset, nullptr) ||
      bookFile.position() != lutOffset) {
    LOG_DBG("BMC", "Cache metadata is truncated or malformed");
    return fail(LoadStatus::Invalid);
  }

  const uint32_t entryCount = static_cast<uint32_t>(spineCount) + tocCount;
  if (entryCount == 0) {
    if (dataStart != dataEnd) {
      LOG_DBG("BMC", "Cache has unreferenced entry data");
      return fail(LoadStatus::Invalid);
    }
  } else {
    // Validate LUT offsets in small sequential chunks. This avoids thousands
    // of alternating LUT/data seeks for large anthologies while keeping RAM
    // bounded to 260 bytes.
    constexpr size_t LUT_CHUNK_SIZE = 64;
    std::array<uint32_t, LUT_CHUNK_SIZE + 1> offsets{};
    uint32_t previousCumulativeSize = 0;
    for (uint32_t base = 0; base < entryCount; base += LUT_CHUNK_SIZE) {
      const size_t chunkCount = std::min<size_t>(LUT_CHUNK_SIZE, entryCount - base);
      const bool hasNextOffset = base + chunkCount < entryCount;
      const size_t offsetCount = chunkCount + (hasNextOffset ? 1 : 0);
      const uint64_t lutPosition = static_cast<uint64_t>(lutOffset) + static_cast<uint64_t>(base) * sizeof(uint32_t);
      if (lutPosition > SIZE_MAX || !bookFile.seek(static_cast<size_t>(lutPosition)) ||
          bookFile.read(offsets.data(), offsetCount * sizeof(uint32_t)) !=
              static_cast<int>(offsetCount * sizeof(uint32_t))) {
        LOG_DBG("BMC", "Cache entry LUT is malformed");
        return fail(LoadStatus::Invalid);
      }

      for (size_t withinChunk = 0; withinChunk < chunkCount; ++withinChunk) {
        const uint32_t index = base + withinChunk;
        const uint32_t entryStart = offsets[withinChunk];
        const uint32_t entryEnd =
            withinChunk + 1 < offsetCount ? offsets[withinChunk + 1] : static_cast<uint32_t>(dataEnd);
        if (!validEntryBounds(lutOffset, entryCount, static_cast<size_t>(dataEnd), entryStart, entryEnd) ||
            (index == 0 && entryStart != dataStart) ||
            (bookFile.position() != entryStart && !bookFile.seek(entryStart))) {
          LOG_DBG("BMC", "Cache entry LUT is malformed");
          return fail(LoadStatus::Invalid);
        }

        if (index < spineCount) {
          uint32_t cumulativeSize = 0;
          if (!inspectSpineEntry(bookFile, entryEnd, tocCount, &cumulativeSize) ||
              cumulativeSize < previousCumulativeSize) {
            LOG_DBG("BMC", "Cache spine entry is malformed");
            return fail(LoadStatus::Invalid);
          }
          previousCumulativeSize = cumulativeSize;
        } else if (!inspectTocEntry(bookFile, entryEnd, spineCount)) {
          LOG_DBG("BMC", "Cache TOC entry is malformed");
          return fail(LoadStatus::Invalid);
        }
      }
    }
  }

  if (!bookFile.seek(BOOK_CACHE_FIXED_HEADER_SIZE) || !consumeBoundedString(bookFile, lutOffset, &coreMetadata.title) ||
      !consumeBoundedString(bookFile, lutOffset, &coreMetadata.author) ||
      !consumeBoundedString(bookFile, lutOffset, &coreMetadata.language) ||
      !consumeBoundedString(bookFile, lutOffset, &coreMetadata.coverItemHref) ||
      !consumeBoundedString(bookFile, lutOffset, &coreMetadata.textReferenceHref) || bookFile.position() != lutOffset) {
    coreMetadata = {};
    return fail(LoadStatus::Invalid);
  }

  dataEndOffset = static_cast<size_t>(dataEnd);
  loadedFileSize = fileSize;
  loaded = true;
  lastLoadStatus = LoadStatus::Loaded;
  LOG_DBG("BMC", "Loaded cache data: %d spine, %d TOC entries", spineCount, tocCount);
  return lastLoadStatus;
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

  const uint32_t entryCount = static_cast<uint32_t>(spineCount) + tocCount;
  size_t entryStart = 0;
  size_t entryEnd = 0;
  SpineEntry entry;
  if (bookFile.size() != loadedFileSize ||
      !readEntryBounds(bookFile, lutOffset, entryCount, static_cast<uint32_t>(index), dataEndOffset, entryStart,
                       entryEnd) ||
      !bookFile.seek(entryStart) || !readSpineEntryChecked(bookFile, entryEnd, tocCount, entry)) {
    LOG_ERR("BMC", "Spine cache changed or became corrupt after load");
    loaded = false;
    lastLoadStatus = LoadStatus::Invalid;
    bookFile.close();
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

  const uint32_t entryCount = static_cast<uint32_t>(spineCount) + tocCount;
  const uint32_t entryIndex = static_cast<uint32_t>(spineCount) + static_cast<uint32_t>(index);
  size_t entryStart = 0;
  size_t entryEnd = 0;
  TocEntry entry;
  if (bookFile.size() != loadedFileSize ||
      !readEntryBounds(bookFile, lutOffset, entryCount, entryIndex, dataEndOffset, entryStart, entryEnd) ||
      !bookFile.seek(entryStart) || !readTocEntryChecked(bookFile, entryEnd, spineCount, entry)) {
    LOG_ERR("BMC", "TOC cache changed or became corrupt after load");
    loaded = false;
    lastLoadStatus = LoadStatus::Invalid;
    bookFile.close();
    return {};
  }
  return entry;
}

BookMetadataCache::SpineEntry BookMetadataCache::readSpineEntry(HalFile& file) const {
  return readSpineEntryFrom(file);
}

BookMetadataCache::TocEntry BookMetadataCache::readTocEntry(HalFile& file) const { return readTocEntryFrom(file); }
