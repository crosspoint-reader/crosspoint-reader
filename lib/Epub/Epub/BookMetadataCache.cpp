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
// Build target for buildBookBin(): written in full, then swapped over book.bin with
// Storage.rename() so a power-loss mid-build can never leave a partially-written
// book.bin behind (see Section::binTmpPath() for the same pattern).
constexpr char bookBinTmpFile[] = "/book.bin.part";
constexpr char tmpSpineBinFile[] = "/spine.bin.tmp";
constexpr char tmpTocBinFile[] = "/toc.bin.tmp";
// Buffer size for the buildBookBin streams. 3 buffers x 4KB, transient (freed on
// return); 4KB = 8 SD sectors per transfer, enough to stop the sector-cache thrash.
constexpr size_t BUILD_IO_BUFFER_SIZE = 4096;

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

// serialization::readString() trusts the on-disk length prefix unconditionally: a torn
// book.bin (version byte flushed, rest truncated/garbage by a power loss mid-write) can
// carry an arbitrary length there, and std::string::resize() to that length aborts the
// process on OOM (bare `new` under -fno-exceptions). Reject any length that would read
// past the file's actual size instead of trusting it.
bool readBoundedString(HalFile& file, std::string& out, const size_t fileSize) {
  uint32_t len;
  serialization::readPod(file, len);
  if (static_cast<uint64_t>(file.position()) + len > static_cast<uint64_t>(fileSize)) {
    return false;
  }
  out.resize(len);
  file.read(&out[0], len);
  return true;
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

bool BookMetadataCache::buildBookBin(const std::string& epubPath, const BookMetadata& metadata) {
  const std::string bookBinFinalPath = cachePath + bookBinFile;
  const std::string bookBinTmpPath = cachePath + bookBinTmpFile;

  // Remove a stale tmp left by a crash-interrupted build; this build recreates it.
  if (Storage.exists(bookBinTmpPath.c_str())) {
    Storage.remove(bookBinTmpPath.c_str());
  }

  // Open all three files, writing to the tmp meta file, reading from spine and toc.
  // The tmp file is swapped over book.bin only after a full, successful write --
  // see the rename at the bottom of this function.
  if (!Storage.openFileForWrite("BMC", bookBinTmpPath, bookFile)) {
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    // Explicit close() required: member variable persists beyond function scope
    bookFile.close();
    Storage.remove(bookBinTmpPath.c_str());
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpTocBinFile, tocFile)) {
    // Explicit close() required: member variables persist beyond function scope
    bookFile.close();
    spineFile.close();
    Storage.remove(bookBinTmpPath.c_str());
    return false;
  }

  // Buffered streams for the whole build: every access below is sequential per
  // file, but interleaved ACROSS files, which thrashes SdFat's single shared
  // sector cache when unbuffered (one 512B SD transaction per 4-byte pod --
  // measured 31s for a 1,732-spine omnibus). Three 4KB buffers, freed on return.
  serialization::BufferedFileWriter bookOut(bookFile, BUILD_IO_BUFFER_SIZE);
  serialization::BufferedFileReader spineIn(spineFile, BUILD_IO_BUFFER_SIZE);
  serialization::BufferedFileReader tocIn(tocFile, BUILD_IO_BUFFER_SIZE);

  constexpr uint32_t headerASize =
      sizeof(BOOK_CACHE_VERSION) + /* LUT Offset */ sizeof(uint32_t) + sizeof(spineCount) + sizeof(tocCount);
  const uint32_t metadataSize = metadata.title.size() + metadata.author.size() + metadata.language.size() +
                                metadata.coverItemHref.size() + metadata.textReferenceHref.size() +
                                sizeof(uint32_t) * 5;
  const uint32_t lutSize = sizeof(uint32_t) * spineCount + sizeof(uint32_t) * tocCount;
  const uint32_t lutOffset = headerASize + metadataSize;

  // Header A. The version byte is stamped as a sentinel (0) here and only patched to the
  // real BOOK_CACHE_VERSION once the whole file is written and flushed successfully (see
  // below) -- that makes the version byte the commit point, matching Section::commitBuildFile.
  // A crash before that patch leaves a tmp file with version 0, which never gets renamed over
  // book.bin, so load() can never observe it.
  constexpr uint8_t SENTINEL_VERSION = 0;
  serialization::writePod(bookOut, SENTINEL_VERSION);
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
      Storage.remove(bookBinTmpPath.c_str());
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
        auto entry = readSpineEntryFrom(spineIn);
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
    auto spineEntry = readSpineEntryFrom(spineIn);
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
    auto tocEntry = readTocEntryFrom(tocIn);
    writeTocEntryTo(bookOut, tocEntry);
  }

  const bool written = bookOut.flush();

  if (written) {
    // Commit point: patch the sentinel version to the real one now that every byte of the
    // tmp file has been written and flushed. Only a fully-written file can ever carry a
    // valid version byte. bookOut is done at this point (buffer drained, fill==0, so its
    // destructor flush is a no-op) — do not write through bookOut after this direct seek.
    bookFile.seek(0);
    serialization::writePod(bookFile, BOOK_CACHE_VERSION);
  }

  // Explicit close() required: member variables persist beyond function scope
  bookFile.close();
  spineFile.close();
  tocFile.close();

  if (!written) {
    // A short write (card full/removed) would leave a truncated tmp file; remove it so the
    // next open rebuilds. book.bin itself is untouched since we never wrote to it directly.
    LOG_ERR("BMC", "Failed writing book.bin, removing truncated tmp file");
    Storage.remove(bookBinTmpPath.c_str());
    return false;
  }

  // Swap the fully-committed tmp file over book.bin. A crash between the remove and the
  // rename loses the old book.bin but leaves a fully-committed tmp behind; the next load()
  // simply fails to find book.bin and rebuilds (see cleanupTmpFiles()/buildBookBin() stale-tmp
  // removal above for the tmp side of that recovery).
  if (Storage.exists(bookBinFinalPath.c_str())) {
    Storage.remove(bookBinFinalPath.c_str());
  }
  if (!Storage.rename(bookBinTmpPath.c_str(), bookBinFinalPath.c_str())) {
    LOG_ERR("BMC", "Failed to move built book.bin into place");
    Storage.remove(bookBinTmpPath.c_str());
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

bool BookMetadataCache::load() {
  if (!Storage.openFileForRead("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  const size_t fileSize = bookFile.fileSize();

  uint8_t version;
  serialization::readPod(bookFile, version);
  if (version != BOOK_CACHE_VERSION) {
    LOG_DBG("BMC", "Cache version mismatch: expected %d, got %d", BOOK_CACHE_VERSION, version);
    // Explicit close() required: member variable persists beyond function scope
    bookFile.close();
    return false;
  }

  serialization::readPod(bookFile, lutOffset);
  serialization::readPod(bookFile, spineCount);
  serialization::readPod(bookFile, tocCount);

  // buildBookBin() only ever stamps a valid version byte after the whole file is written and
  // renamed into place atomically (see buildBookBin()), so this should be unreachable in
  // practice. It stays as a defense against caches built before that fix, or corrupted by
  // other means (e.g. a bad SD sector): reject anything whose LUT wouldn't fit in the actual
  // file rather than trusting it and reading garbage offsets/counts downstream.
  constexpr uint32_t headerASize =
      sizeof(BOOK_CACHE_VERSION) + sizeof(lutOffset) + sizeof(spineCount) + sizeof(tocCount);
  const uint64_t lutSize = sizeof(uint32_t) * (static_cast<uint64_t>(spineCount) + tocCount);
  if (lutOffset < headerASize || lutOffset > fileSize || static_cast<uint64_t>(lutOffset) + lutSize > fileSize) {
    LOG_ERR("BMC", "Cache header inconsistent: lutOffset=%u spineCount=%u tocCount=%u fileSize=%u", lutOffset,
            spineCount, tocCount, static_cast<unsigned>(fileSize));
    // Explicit close() required: member variable persists beyond function scope
    bookFile.close();
    return false;
  }

  if (!readBoundedString(bookFile, coreMetadata.title, fileSize) ||
      !readBoundedString(bookFile, coreMetadata.author, fileSize) ||
      !readBoundedString(bookFile, coreMetadata.language, fileSize) ||
      !readBoundedString(bookFile, coreMetadata.coverItemHref, fileSize) ||
      !readBoundedString(bookFile, coreMetadata.textReferenceHref, fileSize)) {
    LOG_ERR("BMC", "Cache metadata string truncated");
    // Explicit close() required: member variable persists beyond function scope
    bookFile.close();
    return false;
  }

  // Cache cumulative spine sizes in RAM. The progress bar (every render) and percent
  // jumps otherwise pay 2 seeks + a heap-allocating SpineEntry read per access. Spine
  // entries are stored contiguously in index order immediately after the LUTs, so read
  // them in a single sequential pass.
  cumulativeSizes.clear();
  cumulativeSizes.reserve(spineCount);
  // lutSize was computed and bounds-checked against fileSize in the header validation above.
  bookFile.seek(static_cast<size_t>(lutOffset + lutSize));
  for (uint16_t i = 0; i < spineCount; i++) {
    cumulativeSizes.push_back(readSpineEntry(bookFile).cumulativeSize);
  }

  loaded = true;
  LOG_DBG("BMC", "Loaded cache data: %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

uint32_t BookMetadataCache::getCumulativeSize(const int index) const {
  if (index < 0 || index >= static_cast<int>(cumulativeSizes.size())) {
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

BookMetadataCache::SpineEntry BookMetadataCache::readSpineEntry(HalFile& file) const {
  return readSpineEntryFrom(file);
}

BookMetadataCache::TocEntry BookMetadataCache::readTocEntry(HalFile& file) const { return readTocEntryFrom(file); }
