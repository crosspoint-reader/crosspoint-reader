#include "FileIndex.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <NaturalSort.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr const char* INDEX_DIR = "/.crosspoint/fileindex";
constexpr char MAGIC[4] = {'C', 'P', 'F', 'I'};
constexpr uint8_t INDEX_VERSION = 1;
// Run chunk: entries sorted in RAM before being flushed as one sorted run.
// 64 x 32 B = 2 KB, allocated once per build.
constexpr size_t CHUNK_ENTRIES = 64;
constexpr size_t NAME_BUF_SIZE = FileIndex::MAX_NAME + 1;
constexpr uint8_t SECTION_DIR = 1;
constexpr uint8_t SECTION_FILE = 2;
// Largest group of full-key ties we re-order by real name; bigger groups keep
// deterministic (enumeration) order. Ties require 27 identical key bytes, so
// groups beyond this are pathological.
constexpr size_t MAX_TIE_SEGMENT = 64;
constexpr uint32_t FNV32_BASIS = 2166136261u;

uint32_t fnv1a32(const void* data, size_t len, uint32_t hash) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; i++) {
    hash ^= p[i];
    hash *= 16777619u;
  }
  return hash;
}

uint64_t fnv1a64(const char* s) {
  uint64_t hash = 1469598103934665603ULL;
  while (*s) {
    hash ^= static_cast<uint8_t>(*s++);
    hash *= 1099511628211ULL;
  }
  return hash;
}

// FAT timestamp, max(mtime, ctime): files copied onto the card keep their
// original mtime but get a fresh ctime, so the max reflects when the file
// was added to the device. Must match FileBrowserActivity::loadFiles.
uint32_t packDateTime(HalFile& file) {
  uint16_t mdate = 0, mtime = 0, cdate = 0, ctime = 0;
  file.getModifyDateTime(&mdate, &mtime);
  file.getCreateDateTime(&cdate, &ctime);
  const uint32_t modTs = (static_cast<uint32_t>(mdate) << 16) | mtime;
  const uint32_t crtTs = (static_cast<uint32_t>(cdate) << 16) | ctime;
  return modTs > crtTs ? modTs : crtTs;
}

// Feed the task watchdog during long SD loops without meaningfully slowing them
void maybeYield(uint32_t& counter) {
  if ((++counter & 0xFF) == 0) delay(1);
}
}  // namespace

struct FileIndex::BuildState {
  HalFile idxTmp;   // header + path + blob, later offsets (O_RDWR)
  HalFile runsOut;  // run file currently being appended
  char tmpPath[64] = {0};
  char runsPathA[48] = {0};
  char runsPathB[48] = {0};
  const char* finalRunsPath = nullptr;  // scratch file holding the single sorted run
  std::unique_ptr<RunRecord[]> chunk;
  size_t chunkUsed = 0;
  uint32_t runCount = 0;
  uint32_t blobLen = 0;
  std::unique_ptr<char[]> nameA;  // tie-repair name buffers
  std::unique_ptr<char[]> nameB;
  uint32_t segment[MAX_TIE_SEGMENT] = {0};  // tie-repair group; here to stay off the task stack
  uint32_t yieldCounter = 0;
};

bool FileIndex::open(const char* dirPath, SortMode sortMode, AcceptFn accept) {
  close();

  if (!nameBuf) nameBuf = makeUniqueNoThrow<char[]>(NAME_BUF_SIZE);
  if (!nameBuf) {
    LOG_ERR("FIDX", "name buffer alloc failed");
    return false;
  }

  uint32_t signature = 0, dirs = 0, files = 0;
  if (!scanDirectory(dirPath, accept, signature, dirs, files)) {
    return false;
  }

  if (loadExisting(dirPath, sortMode, signature, dirs, files)) {
    LOG_DBG("FIDX", "index valid for %s (%u dirs, %u files)", dirPath, hdr.dirCount, hdr.fileCount);
    return true;
  }

  LOG_INF("FIDX", "building index for %s (%u dirs, %u files)", dirPath, dirs, files);
  return build(dirPath, sortMode, accept, signature, dirs, files);
}

void FileIndex::close() {
  if (idxFile) idxFile.close();
  opened = false;
  offsetsCacheFirst = SIZE_MAX;
  memset(&hdr, 0, sizeof(hdr));
}

bool FileIndex::scanDirectory(const char* dirPath, AcceptFn accept, uint32_t& signature, uint32_t& dirs,
                              uint32_t& files) {
  auto root = Storage.open(dirPath);
  if (!root || !root.isDirectory()) {
    return false;
  }
  root.rewindDirectory();

  uint32_t sig = FNV32_BASIS;
  dirs = 0;
  files = 0;
  uint32_t yieldCounter = 0;

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(nameBuf.get(), NAME_BUF_SIZE);
    const bool isDir = file.isDirectory();
    if (!accept(nameBuf.get(), isDir)) continue;

    const uint32_t size = isDir ? 0 : static_cast<uint32_t>(file.fileSize());
    const uint32_t dateTime = packDateTime(file);
    const uint8_t dirByte = isDir ? 1 : 0;

    sig = fnv1a32(nameBuf.get(), strlen(nameBuf.get()), sig);
    sig = fnv1a32(&size, sizeof(size), sig);
    sig = fnv1a32(&dateTime, sizeof(dateTime), sig);
    sig = fnv1a32(&dirByte, sizeof(dirByte), sig);

    if (isDir) {
      dirs++;
    } else {
      files++;
    }
    maybeYield(yieldCounter);
  }
  root.close();

  signature = sig;
  return true;
}

bool FileIndex::loadExisting(const char* dirPath, SortMode sortMode, uint32_t signature, uint32_t dirs,
                             uint32_t files) {
  snprintf(idxPath, sizeof(idxPath), "%s/%016llx.idx", INDEX_DIR, static_cast<unsigned long long>(fnv1a64(dirPath)));

  auto file = Storage.open(idxPath);
  if (!file) return false;

  IndexHeader h{};
  bool valid = file.read(&h, sizeof(h)) == static_cast<int>(sizeof(h)) && memcmp(h.magic, MAGIC, sizeof(MAGIC)) == 0 &&
               h.version == INDEX_VERSION && h.sortMode == static_cast<uint8_t>(sortMode) &&
               h.dirSignature == signature && h.dirCount == dirs && h.fileCount == files &&
               h.pathLen == strlen(dirPath) && h.pathLen < NAME_BUF_SIZE;

  // Path check guards against a hash collision between two directories
  if (valid) {
    valid = file.read(nameBuf.get(), h.pathLen) == static_cast<int>(h.pathLen) &&
            memcmp(nameBuf.get(), dirPath, h.pathLen) == 0;
  }

  // Length check catches truncated/partial files
  if (valid) {
    const uint64_t expectedSize =
        static_cast<uint64_t>(h.offsetsStart) + static_cast<uint64_t>(h.dirCount + h.fileCount) * sizeof(uint32_t);
    valid = file.fileSize64() == expectedSize;
  }

  if (!valid) {
    file.close();
    return false;
  }

  hdr = h;
  idxFile = std::move(file);
  opened = true;
  return true;
}

void FileIndex::makeKey(RunRecord& rec, SortMode sortMode, bool isDir, uint32_t size, uint32_t dateTime,
                        const char* name) const {
  memset(rec.key, 0, sizeof(rec.key));
  rec.key[0] = isDir ? SECTION_DIR : SECTION_FILE;
  uint8_t* payload = rec.key + 1;
  const size_t payloadCap = sizeof(rec.key) - 1;

  if (sortMode == SortMode::Type) {
    // Fixed 8-byte extension key (covers every real extension; zero-padded so
    // shorter extensions sort first, matching naturalCompare's prefix rule),
    // then the name key breaks ties.
    constexpr size_t EXT_KEY_LEN = 8;
    const char* ext = "";
    if (!isDir) {
      const char* dot = strrchr(name, '.');
      if (dot && dot != name) ext = dot + 1;
    }
    FsHelpers::naturalSortKey(ext, payload, EXT_KEY_LEN);
    FsHelpers::naturalSortKey(name, payload + EXT_KEY_LEN, payloadCap - EXT_KEY_LEN);
    return;
  }

  uint32_t numeric = 0;
  switch (sortMode) {
    case SortMode::Date:
      numeric = dateTime;
      break;
    case SortMode::Size:
      numeric = isDir ? 0 : size;
      break;
    case SortMode::Name:
    default:
      FsHelpers::naturalSortKey(name, payload, payloadCap);
      return;
  }

  // Big-endian so memcmp orders numerically; name key breaks ties
  payload[0] = static_cast<uint8_t>(numeric >> 24);
  payload[1] = static_cast<uint8_t>(numeric >> 16);
  payload[2] = static_cast<uint8_t>(numeric >> 8);
  payload[3] = static_cast<uint8_t>(numeric);
  FsHelpers::naturalSortKey(name, payload + 4, payloadCap - 4);
}

bool FileIndex::flushChunk(BuildState& bs) {
  if (bs.chunkUsed == 0) return true;

  std::sort(bs.chunk.get(), bs.chunk.get() + bs.chunkUsed, [](const RunRecord& a, const RunRecord& b) {
    const int cmp = memcmp(a.key, b.key, sizeof(a.key));
    if (cmp != 0) return cmp < 0;
    return a.blobOffset < b.blobOffset;  // deterministic; full-key ties repaired by name later
  });

  const size_t bytes = bs.chunkUsed * sizeof(RunRecord);
  if (bs.runsOut.write(bs.chunk.get(), bytes) != bytes) {
    LOG_ERR("FIDX", "run write failed");
    return false;
  }
  bs.chunkUsed = 0;
  bs.runCount++;
  return true;
}

bool FileIndex::build(const char* dirPath, SortMode sortMode, AcceptFn accept, uint32_t signature, uint32_t dirs,
                      uint32_t files) {
  (void)signature;
  (void)dirs;
  (void)files;

  if (!Storage.ensureDirectoryExists(INDEX_DIR)) {
    LOG_ERR("FIDX", "cannot create %s", INDEX_DIR);
    return false;
  }

  auto bsPtr = makeUniqueNoThrow<BuildState>();
  if (!bsPtr) return false;
  BuildState& bs = *bsPtr;

  snprintf(bs.tmpPath, sizeof(bs.tmpPath), "%s.tmp", idxPath);
  snprintf(bs.runsPathA, sizeof(bs.runsPathA), "%s/runs.a", INDEX_DIR);
  snprintf(bs.runsPathB, sizeof(bs.runsPathB), "%s/runs.b", INDEX_DIR);

  bs.chunk = makeUniqueNoThrow<RunRecord[]>(CHUNK_ENTRIES);
  bs.nameA = makeUniqueNoThrow<char[]>(NAME_BUF_SIZE);
  bs.nameB = makeUniqueNoThrow<char[]>(NAME_BUF_SIZE);
  if (!bs.chunk || !bs.nameA || !bs.nameB) {
    LOG_ERR("FIDX", "build alloc failed");
    return false;
  }

  auto cleanupScratch = [&bs]() {
    Storage.remove(bs.tmpPath);
    Storage.remove(bs.runsPathA);
    Storage.remove(bs.runsPathB);
  };

  bs.idxTmp = Storage.open(bs.tmpPath, O_RDWR | O_CREAT | O_TRUNC);
  bs.runsOut = Storage.open(bs.runsPathA, O_RDWR | O_CREAT | O_TRUNC);
  if (!bs.idxTmp || !bs.runsOut) {
    LOG_ERR("FIDX", "cannot open scratch files");
    cleanupScratch();
    return false;
  }

  // --- Pass A: enumerate directory, write blob records, emit sorted runs ---
  const uint16_t pathLen = static_cast<uint16_t>(strlen(dirPath));
  const uint32_t blobStart = sizeof(IndexHeader) + pathLen;

  IndexHeader newHdr{};  // zero placeholder; real header written on success
  bool ok = bs.idxTmp.write(&newHdr, sizeof(newHdr)) == sizeof(newHdr) && bs.idxTmp.write(dirPath, pathLen) == pathLen;

  uint32_t sig = FNV32_BASIS;
  uint32_t dirCount = 0, fileCount = 0;

  if (ok) {
    auto root = Storage.open(dirPath);
    if (!root || !root.isDirectory()) {
      ok = false;
    } else {
      root.rewindDirectory();
      for (auto file = root.openNextFile(); ok && file; file = root.openNextFile()) {
        file.getName(nameBuf.get(), NAME_BUF_SIZE);
        const bool isDir = file.isDirectory();
        if (!accept(nameBuf.get(), isDir)) continue;

        const uint32_t size = isDir ? 0 : static_cast<uint32_t>(file.fileSize());
        const uint32_t dateTime = packDateTime(file);
        const uint8_t dirByte = isDir ? 1 : 0;
        const uint16_t nameLen = static_cast<uint16_t>(strlen(nameBuf.get()));

        sig = fnv1a32(nameBuf.get(), nameLen, sig);
        sig = fnv1a32(&size, sizeof(size), sig);
        sig = fnv1a32(&dateTime, sizeof(dateTime), sig);
        sig = fnv1a32(&dirByte, sizeof(dirByte), sig);
        if (isDir) {
          dirCount++;
        } else {
          fileCount++;
        }

        RecordHeader rec{};
        rec.size = size;
        rec.dateTime = dateTime;
        rec.flags = isDir ? 1 : 0;
        rec.nameLen = nameLen;

        RunRecord run;
        run.blobOffset = blobStart + bs.blobLen;
        makeKey(run, sortMode, isDir, size, dateTime, nameBuf.get());

        ok = bs.idxTmp.write(&rec, sizeof(rec)) == sizeof(rec) && bs.idxTmp.write(nameBuf.get(), nameLen) == nameLen;
        bs.blobLen += sizeof(rec) + nameLen;

        bs.chunk[bs.chunkUsed++] = run;
        if (bs.chunkUsed == CHUNK_ENTRIES) ok = ok && flushChunk(bs);
        maybeYield(bs.yieldCounter);
      }
      root.close();
    }
  }
  ok = ok && flushChunk(bs);
  bs.runsOut.flush();
  bs.runsOut.close();

  const uint32_t recordCount = dirCount + fileCount;
  ok = ok && mergeRuns(bs, recordCount);
  ok = ok && writeOffsets(bs, recordCount);

  if (ok) {
    memcpy(newHdr.magic, MAGIC, sizeof(MAGIC));
    newHdr.version = INDEX_VERSION;
    newHdr.sortMode = static_cast<uint8_t>(sortMode);
    newHdr.pathLen = pathLen;
    newHdr.dirSignature = sig;
    newHdr.dirCount = dirCount;
    newHdr.fileCount = fileCount;
    newHdr.blobStart = blobStart;
    newHdr.blobLen = bs.blobLen;
    newHdr.offsetsStart = blobStart + bs.blobLen;
    ok = bs.idxTmp.seek(0) && bs.idxTmp.write(&newHdr, sizeof(newHdr)) == sizeof(newHdr);
    bs.idxTmp.flush();
  }
  bs.idxTmp.close();

  if (!ok) {
    LOG_ERR("FIDX", "index build failed for %s", dirPath);
    cleanupScratch();
    return false;
  }

  // Atomic swap-in; a power loss before the rename just leaves stale scratch
  Storage.remove(idxPath);
  if (!Storage.rename(bs.tmpPath, idxPath)) {
    LOG_ERR("FIDX", "rename to %s failed", idxPath);
    cleanupScratch();
    return false;
  }
  Storage.remove(bs.runsPathA);
  Storage.remove(bs.runsPathB);

  idxFile = Storage.open(idxPath);
  if (!idxFile) {
    LOG_ERR("FIDX", "reopen failed: %s", idxPath);
    return false;
  }
  hdr = newHdr;
  opened = true;
  LOG_INF("FIDX", "index built: %u dirs, %u files", dirCount, fileCount);
  return true;
}

bool FileIndex::mergeRuns(BuildState& bs, uint32_t recordCount) {
  const char* inPath = bs.runsPathA;
  const char* outPath = bs.runsPathB;
  uint32_t runLen = CHUNK_ENTRIES;
  uint32_t runCount = bs.runCount;

  while (runCount > 1) {
    // Two read cursors on the input (read-only, independent positions) and one
    // sequential writer on the other scratch file
    auto inA = Storage.open(inPath);
    auto inB = Storage.open(inPath);
    auto out = Storage.open(outPath, O_RDWR | O_CREAT | O_TRUNC);
    if (!inA || !inB || !out) {
      LOG_ERR("FIDX", "merge pass open failed");
      return false;
    }

    bool ok = true;
    uint32_t newRunCount = 0;
    for (uint32_t run = 0; ok && run < runCount; run += 2) {
      const uint32_t startA = run * runLen;
      const uint32_t lenA = std::min(runLen, recordCount - startA);

      if (run + 1 >= runCount) {
        // Odd run out: copy through unchanged (reuse the chunk as a copy buffer)
        ok = inA.seek(static_cast<size_t>(startA) * sizeof(RunRecord));
        uint32_t left = lenA;
        while (ok && left > 0) {
          const uint32_t batch = std::min<uint32_t>(left, CHUNK_ENTRIES);
          const size_t bytes = batch * sizeof(RunRecord);
          ok = inA.read(bs.chunk.get(), bytes) == static_cast<int>(bytes) && out.write(bs.chunk.get(), bytes) == bytes;
          left -= batch;
          maybeYield(bs.yieldCounter);
        }
        newRunCount++;
        break;
      }

      const uint32_t startB = startA + lenA;
      const uint32_t lenB = std::min(runLen, recordCount - startB);

      ok = inA.seek(static_cast<size_t>(startA) * sizeof(RunRecord)) &&
           inB.seek(static_cast<size_t>(startB) * sizeof(RunRecord));

      RunRecord recA, recB;
      uint32_t ia = 0, ib = 0;
      bool haveA = false, haveB = false;
      while (ok && (ia < lenA || ib < lenB)) {
        if (!haveA && ia < lenA) {
          ok = inA.read(&recA, sizeof(recA)) == static_cast<int>(sizeof(recA));
          haveA = ok;
        }
        if (ok && !haveB && ib < lenB) {
          ok = inB.read(&recB, sizeof(recB)) == static_cast<int>(sizeof(recB));
          haveB = ok;
        }
        if (!ok) break;

        bool takeA;
        if (haveA && haveB) {
          const int cmp = memcmp(recA.key, recB.key, sizeof(recA.key));
          takeA = cmp < 0 || (cmp == 0 && recA.blobOffset <= recB.blobOffset);
        } else {
          takeA = haveA;
        }

        const RunRecord& rec = takeA ? recA : recB;
        ok = out.write(&rec, sizeof(rec)) == sizeof(rec);
        if (takeA) {
          haveA = false;
          ia++;
        } else {
          haveB = false;
          ib++;
        }
        maybeYield(bs.yieldCounter);
      }
      newRunCount++;
    }

    inA.close();
    inB.close();
    out.flush();
    out.close();
    if (!ok) return false;

    std::swap(inPath, outPath);
    runLen *= 2;
    runCount = newRunCount;
  }

  bs.finalRunsPath = inPath;  // holds the single fully sorted run (or pass-A output)
  return true;
}

bool FileIndex::readNameAt(HalFile& file, uint32_t recordOffset, char* out, size_t cap) {
  RecordHeader rec{};
  if (!file.seek(recordOffset) || file.read(&rec, sizeof(rec)) != static_cast<int>(sizeof(rec))) return false;
  const size_t n = std::min<size_t>(rec.nameLen, cap - 1);
  if (file.read(out, n) != static_cast<int>(n)) return false;
  out[n] = '\0';
  return true;
}

bool FileIndex::writeOffsets(BuildState& bs, uint32_t recordCount) {
  // Stream the sorted run and append each blobOffset to the index. Groups of
  // records with identical full keys (truncated-key collisions) are re-ordered
  // with naturalCompare on the real names so the on-SD order matches the
  // in-RAM sort exactly.
  auto run = Storage.open(bs.finalRunsPath);
  if (!run && recordCount > 0) {
    LOG_ERR("FIDX", "cannot open final run");
    return false;
  }

  uint32_t* segment = bs.segment;
  size_t segmentLen = 0;
  bool segmentOverflow = false;
  RunRecord prev{}, cur{};
  bool havePrev = false;
  bool ok = true;
  // Pass A left idxTmp positioned at the end of the blob, where the offsets
  // section starts; track the append position explicitly because the repair
  // reads seek elsewhere in the file.
  uint32_t appendPos = bs.idxTmp.position();

  auto flushSegment = [&]() {
    if (!ok || segmentLen == 0) return;
    if (segmentLen > 1 && !segmentOverflow) {
      // Insertion sort by real name; flush blob writes before seeking to read
      bs.idxTmp.flush();
      for (size_t i = 1; ok && i < segmentLen; i++) {
        const uint32_t candidate = segment[i];
        ok = readNameAt(bs.idxTmp, candidate, bs.nameA.get(), NAME_BUF_SIZE);
        size_t j = i;
        while (ok && j > 0) {
          ok = readNameAt(bs.idxTmp, segment[j - 1], bs.nameB.get(), NAME_BUF_SIZE);
          if (!ok || FsHelpers::naturalCompare(bs.nameB.get(), bs.nameA.get()) <= 0) break;
          segment[j] = segment[j - 1];
          j--;
        }
        segment[j] = candidate;
      }
    }
    if (ok) ok = bs.idxTmp.seek(appendPos);
    for (size_t i = 0; ok && i < segmentLen; i++) {
      ok = bs.idxTmp.write(&segment[i], sizeof(uint32_t)) == sizeof(uint32_t);
    }
    appendPos += segmentLen * sizeof(uint32_t);
    segmentLen = 0;
  };

  for (uint32_t i = 0; ok && i < recordCount; i++) {
    ok = run.read(&cur, sizeof(cur)) == static_cast<int>(sizeof(cur));
    if (!ok) break;

    const bool sameKey = havePrev && memcmp(prev.key, cur.key, sizeof(cur.key)) == 0;
    if (!sameKey) {
      flushSegment();
      segmentOverflow = false;
    } else if (segmentLen == MAX_TIE_SEGMENT) {
      // Oversized tie group: keep deterministic (enumeration) order for the
      // whole group instead of name-repair. Requires 27 identical key bytes
      // across 64+ entries, so effectively pathological.
      segmentOverflow = true;
      flushSegment();
    }
    segment[segmentLen++] = cur.blobOffset;
    prev = cur;
    havePrev = true;
    maybeYield(bs.yieldCounter);
  }
  flushSegment();

  if (run) run.close();
  return ok;
}

bool FileIndex::readOffsetForPhysIndex(size_t physIndex, uint32_t& recordOffset) {
  if (offsetsCacheFirst == SIZE_MAX || physIndex < offsetsCacheFirst ||
      physIndex >= offsetsCacheFirst + OFFSETS_CACHE_ENTRIES) {
    // Load the cache page containing physIndex
    const size_t total = totalCount();
    const size_t first = physIndex - (physIndex % OFFSETS_CACHE_ENTRIES);
    const size_t count = std::min(OFFSETS_CACHE_ENTRIES, total - first);
    if (!idxFile.seek(hdr.offsetsStart + first * sizeof(uint32_t)) ||
        idxFile.read(offsetsCache, count * sizeof(uint32_t)) != static_cast<int>(count * sizeof(uint32_t))) {
      offsetsCacheFirst = SIZE_MAX;
      return false;
    }
    offsetsCacheFirst = first;
  }
  recordOffset = offsetsCache[physIndex - offsetsCacheFirst];
  return true;
}

bool FileIndex::entryAt(size_t row, bool descending, Entry& out) {
  if (!opened || row >= totalCount()) return false;

  // Directories occupy the first rows in both directions; descending reverses
  // order within each section (same behaviour as the in-RAM comparator).
  size_t phys;
  if (row < hdr.dirCount) {
    phys = descending ? (hdr.dirCount - 1 - row) : row;
  } else {
    const size_t fileRow = row - hdr.dirCount;
    phys = hdr.dirCount + (descending ? (hdr.fileCount - 1 - fileRow) : fileRow);
  }

  uint32_t recordOffset = 0;
  if (!readOffsetForPhysIndex(phys, recordOffset)) return false;

  RecordHeader rec{};
  if (!idxFile.seek(recordOffset) || idxFile.read(&rec, sizeof(rec)) != static_cast<int>(sizeof(rec))) return false;

  const size_t n = std::min<size_t>(rec.nameLen, MAX_NAME);
  if (idxFile.read(out.name, n) != static_cast<int>(n)) return false;
  out.name[n] = '\0';
  out.size = rec.size;
  out.dateTime = rec.dateTime;
  out.isDir = (rec.flags & 1) != 0;
  return true;
}

size_t FileIndex::findRowByName(const char* name, bool descending) {
  if (!opened) return SIZE_MAX;

  // Sequential scan of the blob for the record offset, then of the offsets
  // table for its physical rank. Used once per directory entry (pre-selection).
  uint32_t target = 0;
  bool found = false;
  uint32_t pos = hdr.blobStart;
  const uint32_t blobEnd = hdr.blobStart + hdr.blobLen;
  uint32_t yieldCounter = 0;

  if (!idxFile.seek(pos)) return SIZE_MAX;
  while (pos < blobEnd) {
    RecordHeader rec{};
    if (idxFile.read(&rec, sizeof(rec)) != static_cast<int>(sizeof(rec))) return SIZE_MAX;
    const size_t n = std::min<size_t>(rec.nameLen, MAX_NAME);
    if (idxFile.read(nameBuf.get(), n) != static_cast<int>(n)) return SIZE_MAX;
    nameBuf[n] = '\0';
    if (rec.nameLen <= MAX_NAME && strcmp(nameBuf.get(), name) == 0) {
      target = pos;
      found = true;
      break;
    }
    pos += sizeof(rec) + rec.nameLen;
    if (rec.nameLen > MAX_NAME && !idxFile.seek(pos)) return SIZE_MAX;
    maybeYield(yieldCounter);
  }
  if (!found) return SIZE_MAX;

  const size_t total = totalCount();
  if (!idxFile.seek(hdr.offsetsStart)) return SIZE_MAX;
  for (size_t phys = 0; phys < total; phys++) {
    uint32_t off = 0;
    if (idxFile.read(&off, sizeof(off)) != static_cast<int>(sizeof(off))) return SIZE_MAX;
    if (off == target) {
      // The row<->phys mapping is its own inverse within each section
      if (phys < hdr.dirCount) {
        return descending ? (hdr.dirCount - 1 - phys) : phys;
      }
      const size_t filePhys = phys - hdr.dirCount;
      return hdr.dirCount + (descending ? (hdr.fileCount - 1 - filePhys) : filePhys);
    }
    maybeYield(yieldCounter);
  }
  return SIZE_MAX;
}
