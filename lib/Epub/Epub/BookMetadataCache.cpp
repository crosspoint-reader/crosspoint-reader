#include "BookMetadataCache.h"

#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <ZipFile.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <vector>

#include "FsHelpers.h"
#include "SpineHrefMatch.h"

namespace {
// v12: a sparse EPUB 3 nav gives way to an NCX that maps more chapters; a v11 cache holds the sparse TOC
// v13: TOC hrefs that match a spine item by filename only are mapped; a v12 cache holds them as -1
// v14: coverItemHref follows an SVG wrapper to its picture; a v13 cache holds the .svg path
// v15: a named cover missing from the archive no longer blocks the ZIP-name scan; a v14 cache holds the dead path
constexpr uint8_t BOOK_CACHE_VERSION = 15;
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
  if (!Storage.openFileForWrite("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  spineWriter_.emplace(spineFile);
  return true;
}

bool BookMetadataCache::endContentOpfPass() {
  if (spineWriter_.has_value()) {
    spineWriter_->flush();
    spineWriter_.reset();
  }
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
  tocWriter_.emplace(tocFile);
  tocLookupPrepared_ = false;
  tocMappedCount_ = 0;
  resetTocFilenameFallback();
  return true;
}

void BookMetadataCache::resetTocFilenameFallback() {
  tocFolderShiftKnown_ = false;
  tocFilenameScanBudget_ = TOC_FILENAME_SCAN_BUDGET;
}

void BookMetadataCache::prepareTocLookup() {
  tocLookupPrepared_ = true;
  tocScanNextIndex_ = 0;
  tocScanNextOffset_ = 0;

  // Past MAX_ADDRESSABLE_SPINES the index's int16_t spineIndex would wrap and resolve TOC entries
  // to wrong (or negative) spines. The linear scan has no such limit, so fall back to it rather
  // than build a silently-corrupt index. No known EPUB comes close; this is a correctness guard,
  // not a memory one.
  useSpineHrefIndex = false;
  if (spineCount > MAX_ADDRESSABLE_SPINES) {
    LOG_INF("BMC", "Spine count %d exceeds indexable %d; using linear TOC lookup", spineCount, MAX_ADDRESSABLE_SPINES);
  } else if (spineCount >= LARGE_SPINE_THRESHOLD) {
    spineHrefIndex = makeUniqueNoThrow<SpineHrefIndexEntry[]>(spineCount);
    if (!spineHrefIndex) {
      LOG_INF("BMC", "No %u B block for the spine href index (%d spines); resuming linear TOC lookup",
              static_cast<unsigned>(spineCount * sizeof(SpineHrefIndexEntry)), spineCount);
    } else {
      spineFile.seek(0);
      serialization::BufferedFileReader spineReader(spineFile);
      SpineEntry scratch;
      for (int i = 0; i < spineCount; i++) {
        readSpineEntry(spineReader, scratch);
        SpineHrefIndexEntry idx;
        idx.hrefHash = HashUtils::fnvHash64(scratch.href);
        idx.hrefLen = static_cast<uint16_t>(scratch.href.size());
        idx.spineIndex = static_cast<int16_t>(i);
        spineHrefIndex[i] = idx;
      }
      std::sort(spineHrefIndex.get(), spineHrefIndex.get() + spineCount,
                [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                  return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
                });
      useSpineHrefIndex = true;
      LOG_DBG("BMC", "Using fast index for %d spine items", spineCount);
    }
  }
  if (!useSpineHrefIndex) {
    spineFile.seek(0);
    spineScanReader_.emplace(spineFile);
  }
}

int16_t BookMetadataCache::findSpineByScan(const std::string& href) {
  SpineEntry entry;
  // From the resume point to the end, then from the first spine up to the resume point.
  const int resumeIndex = tocScanNextIndex_;
  for (int pass = 0; pass < 2; pass++) {
    const int first = pass == 0 ? resumeIndex : 0;
    const int last = pass == 0 ? spineCount : resumeIndex;
    spineScanReader_->seek(pass == 0 ? tocScanNextOffset_ : 0);
    for (int i = first; i < last; i++) {
      readSpineEntry(*spineScanReader_, entry);
      if (entry.href == href) {
        tocScanNextIndex_ = i + 1;
        tocScanNextOffset_ = spineScanReader_->position();
        if (tocScanNextIndex_ >= spineCount) {
          tocScanNextIndex_ = 0;
          tocScanNextOffset_ = 0;
        }
        return static_cast<int16_t>(i);
      }
    }
  }
  return -1;
}

bool BookMetadataCache::resetTocPassOutput() {
  if (!buildMode) {
    LOG_DBG("BMC", "resetTocPassOutput called but not in build mode");
    return false;
  }

  tocWriter_.reset();  // discard buffered records from the abandoned parse
  if (tocFile) {
    tocFile.close();
  }

  if (!Storage.openFileForWrite("BMC", cachePath + tmpTocBinFile, tocFile)) {
    LOG_ERR("BMC", "Could not reopen TOC temp file for reset");
    return false;
  }
  tocWriter_.emplace(tocFile);

  tocCount = 0;
  tocMappedCount_ = 0;
  // The fallback parse restarts the TOC from its first entry; scan from the first spine again.
  tocScanNextIndex_ = 0;
  tocScanNextOffset_ = 0;
  resetTocFilenameFallback();
  LOG_DBG("BMC", "Reset TOC temp output for fallback parse");
  return true;
}

bool BookMetadataCache::endTocPass() {
  if (tocWriter_.has_value()) {
    tocWriter_->flush();
    tocWriter_.reset();
  }
  spineScanReader_.reset();  // views spineFile, so dropped before it closes
  tocFile.close();
  spineFile.close();

  spineHrefIndex.reset();
  useSpineHrefIndex = false;
  tocLookupPrepared_ = false;

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
  LOG_DBG("BMC", "buildBookBin start: free=%lu contig=%lu", static_cast<unsigned long>(esp_get_free_heap_size()),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
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

  constexpr uint32_t headerASize = sizeof(BOOK_CACHE_VERSION) + /* LUT Offset */ sizeof(uint32_t) + sizeof(spineCount) +
                                   sizeof(tocCount) + sizeof(uint8_t) /* tocReliable */;
  const uint32_t metadataSize = metadata.title.size() + metadata.author.size() + metadata.language.size() +
                                metadata.coverItemHref.size() + metadata.textReferenceHref.size() +
                                metadata.series.size() + metadata.seriesIndex.size() + metadata.description.size() +
                                sizeof(uint32_t) * 8;
  const uint32_t lutSize = sizeof(uint32_t) * spineCount + sizeof(uint32_t) * tocCount;
  const uint32_t lutOffset = headerASize + metadataSize;

  // Buffered views for the whole compile: the loops below issue tens of thousands of tiny
  // field reads/writes (measured 22.6 s of pure FsFile call overhead for a 1732-spine book);
  // batching them into 4 KB transfers is the entire fix. 3 x 4 KB heap for the duration of
  // the build only — inside the first-open released-framebuffer window. Each wrapper
  // degrades to pass-through if its buffer cannot be allocated.
  serialization::BufferedFileWriter bookWriter(bookFile);
  serialization::BufferedFileReader spineReader(spineFile);
  serialization::BufferedFileReader tocReader(tocFile);

  // Header A. tocReliable is patched at the end once the TOC scan below has computed it.
  const uint32_t tocReliableHeaderPos =
      sizeof(BOOK_CACHE_VERSION) + sizeof(uint32_t) /* lutOffset */ + sizeof(spineCount) + sizeof(tocCount);
  bookWriter.writePod(BOOK_CACHE_VERSION);
  bookWriter.writePod(lutOffset);
  bookWriter.writePod(spineCount);
  bookWriter.writePod(tocCount);
  bookWriter.writePod(static_cast<uint8_t>(0));  // placeholder for tocReliable
  // Metadata
  bookWriter.writeString(metadata.title);
  bookWriter.writeString(metadata.author);
  bookWriter.writeString(metadata.language);
  bookWriter.writeString(metadata.coverItemHref);
  bookWriter.writeString(metadata.textReferenceHref);
  bookWriter.writeString(metadata.series);
  bookWriter.writeString(metadata.seriesIndex);
  bookWriter.writeString(metadata.description);

  // Scratch entries reused across all read loops below. Their internal
  // std::strings keep the capacity of the longest href/title encountered, so
  // each subsequent readSpineEntry / readTocEntry call reuses that allocation
  // instead of churning hundreds of small heap blocks during the build.
  SpineEntry spineScratch;
  TocEntry tocScratch;

  // Loop through spine entries, writing LUT positions
  spineReader.seek(0);
  for (int i = 0; i < spineCount; i++) {
    uint32_t pos = spineReader.position();
    readSpineEntry(spineReader, spineScratch);
    bookWriter.writePod(pos + lutOffset + lutSize);
  }
  // Total bytes of spine records — the offset the TOC records start at in book.bin.
  const uint32_t spineBytesTotal = spineReader.position();

  // Loop through toc entries, writing LUT positions
  tocReader.seek(0);
  for (int i = 0; i < tocCount; i++) {
    uint32_t pos = tocReader.position();
    readTocEntry(tocReader, tocScratch);
    bookWriter.writePod(pos + lutOffset + lutSize + spineBytesTotal);
  }

  // LUTs complete
  // Loop through spines from spine file matching up TOC indexes, calculating cumulative size and writing to book.bin

  // Build spineIndex->tocIndex mapping in one pass (O(n) instead of O(n*m)).
  // Also count distinct spines referenced by the TOC so tocReliable can be persisted in the
  // header below — without this, every first-page load on a large book pays an O(tocCount)
  // seek-heavy scan in Epub::hasReliableToc().
  //
  // Deque, not vector: only ~3.5 KB at 1732 spines, but a vector demands it as ONE contiguous
  // block, and bare operator new abort()s under -fno-exceptions on a fragmented heap — the same
  // failure class the targets/spineSizes deques below were introduced to avoid. This was the one
  // spine-sized table here that never got that treatment.
  std::deque<int16_t> spineToTocIndex(spineCount, -1);
  int distinctSpinesReferenced = 0;
  tocReader.seek(0);
  for (int j = 0; j < tocCount; j++) {
    readTocEntry(tocReader, tocScratch);
    if (tocScratch.spineIndex >= 0 && tocScratch.spineIndex < spineCount) {
      if (spineToTocIndex[tocScratch.spineIndex] == -1) {
        spineToTocIndex[tocScratch.spineIndex] = static_cast<int16_t>(j);
        distinctSpinesReferenced++;
      }
    }
  }

  // Mirrors the heuristic in Epub::hasReliableToc(): require >=25% distinct spine coverage,
  // with short-circuits for edge cases (no entries, or large book with a single TOC entry).
  if (spineCount > 0 && tocCount > 0 && !(spineCount >= 8 && tocCount <= 1)) {
    tocReliable = (distinctSpinesReferenced * 4 >= spineCount);
  } else {
    tocReliable = false;
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

  // Deques, not vectors: at 1732 spines `targets` alone is ~28 KB, and a vector demands that as
  // one contiguous block — observed aborting here at 51 KB free / 30 KB contig before the
  // first-open framebuffer release widened the headroom. Chunked storage removes the CONTIGUITY
  // failure class, but deque still allocates through the throwing path, so total exhaustion would
  // abort() under -fno-exceptions — which is what the heap probe below guards.
  std::deque<uint32_t> spineSizes;
  bool useBatchSizes = false;

  // Batch tables cost ~20 B/spine (16 B SizeTarget + 4 B size): 35 KB at 1732 spines, against
  // ~122 KB free here (device-measured on King's Avatar). Skip the batch path only when the heap
  // genuinely cannot hold them, leaving 32 KB for the ZIP scan and the rest of the build.
  //
  // Sized against what ACTUALLY allocates, deliberately: an earlier 2x-plus-16KB version demanded
  // 216 KB at 5000 spines — more than this device ever has free — so it would have silently
  // dropped plausible ~3000-spine books to the per-item ZIP lookup, i.e. one central-directory
  // scan PER SPINE. That is the O(n) cost the per-book spine-stat cache exists to eliminate, so an
  // over-eager probe here is a performance regression, not a safety net. The fallback is correct
  // (it is the small-book path), just slow — it should trigger only when the alternative is death.
  const size_t batchTableBytes = static_cast<size_t>(spineCount) * (sizeof(ZipFile::SizeTarget) + sizeof(uint32_t));
  const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const bool heapAllowsBatchSizes = freeHeap > batchTableBytes + 32 * 1024;

  if (spineCount >= LARGE_SPINE_THRESHOLD && heapAllowsBatchSizes) {
    LOG_DBG("BMC", "Using batch size lookup for %d spine items", spineCount);

    std::deque<ZipFile::SizeTarget> targets;
    targets.resize(spineCount);

    std::string pathScratch;
    spineReader.seek(0);
    for (int i = 0; i < spineCount; i++) {
      readSpineEntry(spineReader, spineScratch);
      // Cached hrefs are already decoded (see ContentOpfParser); decoding again would corrupt
      // filenames that legitimately contain '%' sequences.
      FsHelpers::normalisePath(spineScratch.href, pathScratch);

      ZipFile::SizeTarget t;
      t.hash = HashUtils::fnvHash64(pathScratch.c_str(), pathScratch.size());
      t.len = static_cast<uint16_t>(pathScratch.size());
      t.index = static_cast<uint16_t>(i);
      targets[i] = t;
    }

    std::sort(targets.begin(), targets.end(), [](const ZipFile::SizeTarget& a, const ZipFile::SizeTarget& b) {
      return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
    });

    spineSizes.resize(spineCount, 0);
    const int matched = zip.fillUncompressedSizes(targets, spineSizes);
    LOG_DBG("BMC", "Batch lookup matched %d/%d spine items", matched, spineCount);
    (void)matched;  // only read by LOG_DBG, which compiles out at lower log levels

    targets.clear();
    targets.shrink_to_fit();

    useBatchSizes = true;
  } else if (spineCount >= LARGE_SPINE_THRESHOLD) {
    // Should be rare — see the sizing note above. If this shows up on an ordinary book, the
    // threshold is too conservative and is costing a central-directory scan per spine.
    LOG_INF("BMC", "Skipping batch size lookup for %d spine items (free %u < %u needed); using per-item lookup",
            spineCount, static_cast<unsigned>(freeHeap), static_cast<unsigned>(batchTableBytes + 32 * 1024));
  }

  uint32_t cumSize = 0;
  spineReader.seek(0);
  int lastSpineTocIndex = -1;
  std::string pathScratch;
  for (int i = 0; i < spineCount; i++) {
    readSpineEntry(spineReader, spineScratch);

    spineScratch.tocIndex = spineToTocIndex[i];

    // Not a huge deal if we don't fine a TOC entry for the spine entry, this is expected behaviour for EPUBs
    // Logging here is for debugging
    if (spineScratch.tocIndex == -1) {
      LOG_DBG("BMC", "Warning: Could not find TOC entry for spine item %d: %s, using title from last section", i,
              spineScratch.href.c_str());
      spineScratch.tocIndex = lastSpineTocIndex;
    }
    lastSpineTocIndex = spineScratch.tocIndex;

    size_t itemSize = 0;
    if (useBatchSizes) {
      itemSize = spineSizes[i];
      if (itemSize == 0) {
        FsHelpers::normalisePath(spineScratch.href, pathScratch);
        if (!zip.getInflatedFileSize(pathScratch.c_str(), &itemSize)) {
          LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", pathScratch.c_str());
        }
      }
    } else {
      FsHelpers::normalisePath(spineScratch.href, pathScratch);
      if (!zip.getInflatedFileSize(pathScratch.c_str(), &itemSize)) {
        LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", pathScratch.c_str());
      }
    }

    cumSize += itemSize;
    spineScratch.cumulativeSize = cumSize;

    // Write out spine data to book.bin
    writeSpineEntry(bookWriter, spineScratch);
  }
  // Close opened zip file
  zip.close();

  // Loop through toc entries from toc file writing to book.bin
  tocReader.seek(0);
  for (int i = 0; i < tocCount; i++) {
    readTocEntry(tocReader, tocScratch);
    writeTocEntry(bookWriter, tocScratch);
  }

  // Patch tocReliable placeholder in header A. Everything buffered must be on disk before the
  // out-of-band seek writes through the raw file handle.
  bookWriter.flush();
  bookFile.seek(tocReliableHeaderPos);
  serialization::writePod(bookFile, static_cast<uint8_t>(tocReliable ? 1 : 0));

  bookFile.close();
  spineFile.close();
  tocFile.close();

  LOG_DBG("BMC", "Successfully built book.bin");
  LOG_DBG("BMC", "buildBookBin end: free=%lu contig=%lu", static_cast<unsigned long>(esp_get_free_heap_size()),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
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

uint32_t BookMetadataCache::writeSpineEntry(serialization::BufferedFileWriter& out, const SpineEntry& entry) const {
  const uint32_t pos = out.position();
  out.writeString(entry.href);
  out.writePod(entry.cumulativeSize);
  out.writePod(entry.tocIndex);
  return pos;
}

uint32_t BookMetadataCache::writeTocEntry(serialization::BufferedFileWriter& out, const TocEntry& entry) const {
  const uint32_t pos = out.position();
  out.writeString(entry.title);
  out.writeString(entry.href);
  out.writeString(entry.anchor);
  out.writePod(entry.level);
  out.writePod(entry.spineIndex);
  return pos;
}

void BookMetadataCache::readSpineEntry(serialization::BufferedFileReader& in, SpineEntry& out) const {
  in.readString(out.href);
  in.readPod(out.cumulativeSize);
  in.readPod(out.tocIndex);
}

void BookMetadataCache::readTocEntry(serialization::BufferedFileReader& in, TocEntry& out) const {
  in.readString(out.title);
  in.readString(out.href);
  in.readString(out.anchor);
  in.readPod(out.level);
  in.readPod(out.spineIndex);
}

// Note: for the LUT to be accurate, this **MUST** be called for all spine items before `addTocEntry` is ever called
// this is because in this function we're marking positions of the items
void BookMetadataCache::createSpineEntry(const std::string& href) {
  if (!buildMode || !spineFile || !spineWriter_.has_value()) {
    LOG_DBG("BMC", "createSpineEntry called but not in build mode");
    return;
  }

  const SpineEntry entry(href, 0, -1);
  writeSpineEntry(*spineWriter_, entry);
  spineCount++;
}

int16_t BookMetadataCache::findSpineExact(const std::string& href) {
  if (!useSpineHrefIndex) return findSpineByScan(href);

  const uint64_t targetHash = HashUtils::fnvHash64(href);
  const uint16_t targetLen = static_cast<uint16_t>(href.size());
  const SpineHrefIndexEntry* const indexBegin = spineHrefIndex.get();
  const SpineHrefIndexEntry* const indexEnd = indexBegin + spineCount;
  const auto it =
      std::lower_bound(indexBegin, indexEnd, SpineHrefIndexEntry{targetHash, targetLen, 0},
                       [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                         return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
                       });
  if (it != indexEnd && it->hrefHash == targetHash && it->hrefLen == targetLen) return it->spineIndex;
  return -1;
}

// A TOC href that no spine href matches exactly can still name a spine item by its filename, as
// Epub::resolveHrefToSpineIndex accepts for in-book links. Books ship nav documents whose links are
// relative to a folder the nav does not sit in (nav.xhtml in OEBPS/ linking "c1.xhtml" for
// OEBPS/text/c1.xhtml): every entry missed, and the contents list showed titles that could not be
// opened. A filename that several spine items share in different folders is not guessed at; that
// entry stays unmapped.
//
// Such a document is off by the same folder throughout, so the folder the last match was found in
// is tried first for the next miss from the same folder, as an exact lookup: a TOC whose every
// entry misses reads the spine file once, not once per entry, and no per-spine table is built. The
// full reads that remain are budgeted per parse, so hrefs that match nothing at all cannot cost a
// read of the whole spine each on a book with thousands of spine items.
//
// Based on crosspoint-reader PR #2987 (Hoang Manh Linh, @jetaudio), reworked to scan the spine
// temp file instead of building a RAM index of every spine filename.
int16_t BookMetadataCache::findSpineByFilename(const std::string& href) {
  const std::string_view name = spine_href::filenameOf(href);
  if (name.empty()) return -1;
  const std::string_view dir = std::string_view(href).substr(0, href.size() - name.size());

  if (tocFolderShiftKnown_ && dir == tocMissDir_) {
    tocShiftedHref_.assign(tocFoundDir_).append(name);
    const int16_t shifted = findSpineExact(tocShiftedHref_);
    if (shifted >= 0) return shifted;
  }

  if (tocFilenameScanBudget_ < spineCount) {
    if (tocFilenameScanBudget_ > 0) {
      LOG_INF("BMC", "TOC filename fallback spent its spine reads (%d spines); later misses stay unmapped", spineCount);
      tocFilenameScanBudget_ = 0;
    }
    return -1;
  }
  tocFilenameScanBudget_ -= spineCount;

  // The resumed scan seeks its own position before every read, so its view can be shared.
  if (!spineScanReader_.has_value()) spineScanReader_.emplace(spineFile);
  spineScanReader_->seek(0);
  SpineEntry entry;
  int16_t found = -1;
  int sameName = 0;
  std::string foundDir;
  for (int i = 0; i < spineCount; i++) {
    readSpineEntry(*spineScanReader_, entry);
    if (spine_href::match(href, entry.href) == SpineHrefMatch::None) continue;
    if (sameName++ == 0) {
      found = static_cast<int16_t>(i);
      foundDir.assign(entry.href, 0, entry.href.size() - name.size());
    }
  }
  if (sameName != 1) {
    if (sameName > 1) {
      LOG_DBG("BMC", "TOC href %s: %d spine items share its filename, left unmapped", href.c_str(), sameName);
    }
    return -1;
  }

  tocMissDir_.assign(dir);
  tocFoundDir_ = std::move(foundDir);
  tocFolderShiftKnown_ = true;
  return found;
}

void BookMetadataCache::createTocEntry(const std::string& title, const std::string& href, const std::string& anchor,
                                       const uint8_t level) {
  if (!buildMode || !tocFile || !spineFile || !tocWriter_.has_value()) {
    LOG_DBG("BMC", "createTocEntry called but not in build mode");
    return;
  }

  // Prepared on the first entry rather than in beginTocPass(): by now the TOC parser and its
  // inflate ring hold their memory, so the optional index can only take what they left. Built
  // first, it once left the NCX parser no room to start, and the book was cached with no TOC.
  if (!tocLookupPrepared_) prepareTocLookup();

  int16_t spineIndex = findSpineExact(href);
  if (spineIndex == -1) spineIndex = findSpineByFilename(href);
  if (spineIndex == -1) {
    LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
  }

  const TocEntry entry(title, href, anchor, level, spineIndex);
  writeTocEntry(*tocWriter_, entry);
  tocCount++;
  if (spineIndex >= 0) tocMappedCount_++;
}

/* ============= READING / LOADING FUNCTIONS ================ */

bool BookMetadataCache::cacheExists(const std::string& cachePath) {
  return Storage.exists((cachePath + bookBinFile).c_str());
}

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
  uint8_t tocReliableByte;
  serialization::readPod(bookFile, tocReliableByte);
  tocReliable = (tocReliableByte != 0);

  serialization::readString(bookFile, coreMetadata.title);
  serialization::readString(bookFile, coreMetadata.author);
  serialization::readString(bookFile, coreMetadata.language);
  serialization::readString(bookFile, coreMetadata.coverItemHref);
  serialization::readString(bookFile, coreMetadata.textReferenceHref);
  serialization::readString(bookFile, coreMetadata.series);
  serialization::readString(bookFile, coreMetadata.seriesIndex);
  serialization::readString(bookFile, coreMetadata.description);

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

void BookMetadataCache::readSpineEntry(FsFile& file, SpineEntry& out) const {
  serialization::readString(file, out.href);
  serialization::readPod(file, out.cumulativeSize);
  serialization::readPod(file, out.tocIndex);
}

void BookMetadataCache::readTocEntry(FsFile& file, TocEntry& out) const {
  serialization::readString(file, out.title);
  serialization::readString(file, out.href);
  serialization::readString(file, out.anchor);
  serialization::readPod(file, out.level);
  serialization::readPod(file, out.spineIndex);
}

BookMetadataCache::SpineEntry BookMetadataCache::readSpineEntry(FsFile& file) const {
  SpineEntry entry;
  readSpineEntry(file, entry);
  return entry;
}

BookMetadataCache::TocEntry BookMetadataCache::readTocEntry(FsFile& file) const {
  TocEntry entry;
  readTocEntry(file, entry);
  return entry;
}
