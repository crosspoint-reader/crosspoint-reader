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
constexpr uint8_t INDEX_VERSION = 2;
// Run chunk: entries sorted in RAM before being flushed as one sorted run.
// 64 x 32 B = 2 KB, allocated once per build.
constexpr size_t CHUNK_ENTRIES = 64;
constexpr size_t NAME_BUF_SIZE = FileIndex::MAX_NAME + 1;
constexpr size_t CHUNK_BYTES = CHUNK_ENTRIES * 32;
constexpr const char* TIES_PATH_A = "/.crosspoint/fileindex/ties.a";
constexpr const char* TIES_PATH_B = "/.crosspoint/fileindex/ties.b";
constexpr uint8_t SECTION_DIR = 1;
constexpr uint8_t SECTION_FILE = 2;
constexpr uint32_t FNV32_BASIS = 2166136261u;

struct TieRunHeader {
  uint32_t count;
  uint32_t bodyBytes;
};

struct TieRecordHeader {
  uint32_t blobOffset;
  uint16_t nameLen;
  uint16_t reserved;
};

struct TieCursor {
  HalFile* file;
  char* name;
  TieRecordHeader record{};
  uint32_t remaining;
  uint32_t bodyBytesRemaining;
  bool loaded = false;
};

struct TieOutputBuffer {
  HalFile* file;
  uint8_t* data;
  size_t used = 0;
};

static_assert(sizeof(TieRunHeader) == 8, "tie run header size");
static_assert(sizeof(TieRecordHeader) == 8, "tie record header size");
static_assert(sizeof(TieRunHeader) + 2 * (sizeof(TieRecordHeader) + FileIndex::MAX_NAME) <= CHUNK_BYTES,
              "two maximum-length tie records must fit in the run chunk");

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

bool writeExact(HalFile& file, const void* data, size_t bytes, const char* what) {
  if (file.write(data, bytes) == bytes) return true;
  LOG_ERR("FIDX", "%s write failed", what);
  return false;
}

bool writeBuffered(TieOutputBuffer& out, const void* data, size_t bytes) {
  if (bytes > CHUNK_BYTES) {
    LOG_ERR("FIDX", "tie buffered write too large: %u", static_cast<unsigned>(bytes));
    return false;
  }
  if (out.used + bytes > CHUNK_BYTES) {
    if (!writeExact(*out.file, out.data, out.used, "tie merge")) return false;
    out.used = 0;
  }
  memcpy(out.data + out.used, data, bytes);
  out.used += bytes;
  return true;
}

bool flushBuffered(TieOutputBuffer& out) {
  if (out.used == 0) return true;
  if (!writeExact(*out.file, out.data, out.used, "tie merge")) return false;
  out.used = 0;
  return true;
}

bool validateTieRun(const TieRunHeader& header, uint64_t bodyStart, uint64_t fileSize) {
  const uint64_t minimumBytes = static_cast<uint64_t>(header.count) * sizeof(TieRecordHeader);
  if (header.count == 0 || header.bodyBytes < minimumBytes || bodyStart + header.bodyBytes > fileSize) {
    LOG_ERR("FIDX", "invalid tie run: count=%u bytes=%u", header.count, header.bodyBytes);
    return false;
  }
  return true;
}

bool loadTieRecord(TieCursor& cursor) {
  if (cursor.loaded || cursor.remaining == 0) return true;
  if (cursor.bodyBytesRemaining < sizeof(TieRecordHeader) ||
      cursor.file->read(&cursor.record, sizeof(cursor.record)) != static_cast<int>(sizeof(cursor.record))) {
    LOG_ERR("FIDX", "tie record header read failed");
    return false;
  }
  cursor.bodyBytesRemaining -= sizeof(TieRecordHeader);
  if (cursor.record.reserved != 0 || cursor.record.nameLen > FileIndex::MAX_NAME ||
      cursor.record.nameLen > cursor.bodyBytesRemaining) {
    LOG_ERR("FIDX", "invalid tie record: name=%u remaining=%u", cursor.record.nameLen, cursor.bodyBytesRemaining);
    return false;
  }
  if (cursor.file->read(cursor.name, cursor.record.nameLen) != static_cast<int>(cursor.record.nameLen)) {
    LOG_ERR("FIDX", "tie name read failed");
    return false;
  }
  cursor.bodyBytesRemaining -= cursor.record.nameLen;
  cursor.name[cursor.record.nameLen] = '\0';
  cursor.loaded = true;
  return true;
}

bool writeTieRecord(TieOutputBuffer& out, const TieCursor& cursor) {
  return writeBuffered(out, &cursor.record, sizeof(cursor.record)) &&
         writeBuffered(out, cursor.name, cursor.record.nameLen);
}

void consumeTieRecord(TieCursor& cursor) {
  cursor.loaded = false;
  cursor.remaining--;
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
  std::unique_ptr<char[]> nameA;  // full-name comparison buffers
  std::unique_ptr<char[]> nameB;
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
    return a.blobOffset < b.blobOffset;
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
  Storage.remove(TIES_PATH_A);
  Storage.remove(TIES_PATH_B);

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
    Storage.remove(TIES_PATH_A);
    Storage.remove(TIES_PATH_B);
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
  if (ok && !bs.idxTmp.seek(blobStart + bs.blobLen)) {
    LOG_ERR("FIDX", "offsets seek failed");
    ok = false;
  }
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
  Storage.remove(TIES_PATH_A);
  Storage.remove(TIES_PATH_B);

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

  bs.finalRunsPath = inPath;  // single run ordered by fixed key, then blob offset
  return true;
}

bool FileIndex::readNameAt(HalFile& file, uint32_t recordOffset, char* out, size_t cap, uint16_t* nameLen) {
  RecordHeader rec{};
  if (!file.seek(recordOffset) || file.read(&rec, sizeof(rec)) != static_cast<int>(sizeof(rec))) {
    LOG_ERR("FIDX", "name header read failed at %u", recordOffset);
    return false;
  }
  if (rec.nameLen >= cap) {
    LOG_ERR("FIDX", "name too long at %u: %u", recordOffset, rec.nameLen);
    return false;
  }
  if (file.read(out, rec.nameLen) != static_cast<int>(rec.nameLen)) {
    LOG_ERR("FIDX", "name read failed at %u", recordOffset);
    return false;
  }
  out[rec.nameLen] = '\0';
  if (nameLen) *nameLen = rec.nameLen;
  return true;
}

bool FileIndex::sortSmallTieGroup(BuildState& bs, size_t count, uint32_t& appendPos) {
  if (count > 1) {
    bs.idxTmp.flush();
    for (size_t i = 1; i < count; i++) {
      const RunRecord candidate = bs.chunk[i];
      if (!readNameAt(bs.idxTmp, candidate.blobOffset, bs.nameA.get(), NAME_BUF_SIZE)) return false;

      size_t first = 0;
      size_t last = i;
      while (first < last) {
        const size_t middle = first + (last - first) / 2;
        const RunRecord& current = bs.chunk[middle];
        if (!readNameAt(bs.idxTmp, current.blobOffset, bs.nameB.get(), NAME_BUF_SIZE)) return false;
        const int nameCmp = FsHelpers::naturalCompare(bs.nameB.get(), bs.nameA.get());
        const bool currentBefore = nameCmp < 0 || (nameCmp == 0 && current.blobOffset < candidate.blobOffset);
        if (currentBefore) {
          first = middle + 1;
        } else {
          last = middle;
        }
      }
      std::move_backward(bs.chunk.get() + first, bs.chunk.get() + i, bs.chunk.get() + i + 1);
      bs.chunk[first] = candidate;
    }

    if (!bs.idxTmp.seek(appendPos)) {
      LOG_ERR("FIDX", "offset append seek failed");
      return false;
    }
  }

  for (size_t i = 0; i < count; i++) {
    if (!writeExact(bs.idxTmp, &bs.chunk[i].blobOffset, sizeof(uint32_t), "offset")) return false;
  }
  appendPos += count * sizeof(uint32_t);
  return true;
}

bool FileIndex::writeInitialTieRun(BuildState& bs, HalFile& out, uint32_t offsetA, bool hasB, uint32_t offsetB,
                                   bool useChunk) {
  uint16_t lenA = 0;
  uint16_t lenB = 0;
  if (!readNameAt(bs.idxTmp, offsetA, bs.nameA.get(), NAME_BUF_SIZE, &lenA) ||
      (hasB && !readNameAt(bs.idxTmp, offsetB, bs.nameB.get(), NAME_BUF_SIZE, &lenB))) {
    return false;
  }

  TieRecordHeader recordA{offsetA, lenA, 0};
  TieRecordHeader recordB{offsetB, lenB, 0};
  bool takeA = true;
  if (hasB) {
    const int cmp = FsHelpers::naturalCompare(bs.nameA.get(), bs.nameB.get());
    takeA = cmp < 0 || (cmp == 0 && offsetA <= offsetB);
  }

  const uint32_t bodyBytes = sizeof(TieRecordHeader) + lenA + (hasB ? sizeof(TieRecordHeader) + lenB : 0);
  const TieRunHeader runHeader{hasB ? 2u : 1u, bodyBytes};
  const TieRecordHeader& firstRecord = takeA ? recordA : recordB;
  const TieRecordHeader& secondRecord = takeA ? recordB : recordA;
  const char* firstName = takeA ? bs.nameA.get() : bs.nameB.get();
  const char* secondName = takeA ? bs.nameB.get() : bs.nameA.get();

  if (!useChunk) {
    return writeExact(out, &runHeader, sizeof(runHeader), "initial tie run") &&
           writeExact(out, &firstRecord, sizeof(firstRecord), "initial tie record") &&
           writeExact(out, firstName, firstRecord.nameLen, "initial tie name") &&
           (!hasB || (writeExact(out, &secondRecord, sizeof(secondRecord), "initial tie record") &&
                      writeExact(out, secondName, secondRecord.nameLen, "initial tie name")));
  }

  uint8_t* bytes = reinterpret_cast<uint8_t*>(bs.chunk.get());
  size_t used = 0;
  memcpy(bytes + used, &runHeader, sizeof(runHeader));
  used += sizeof(runHeader);
  memcpy(bytes + used, &firstRecord, sizeof(firstRecord));
  used += sizeof(firstRecord);
  memcpy(bytes + used, firstName, firstRecord.nameLen);
  used += firstRecord.nameLen;
  if (hasB) {
    memcpy(bytes + used, &secondRecord, sizeof(secondRecord));
    used += sizeof(secondRecord);
    memcpy(bytes + used, secondName, secondRecord.nameLen);
    used += secondRecord.nameLen;
  }
  return writeExact(out, bytes, used, "initial tie run");
}

bool FileIndex::mergeTieRuns(BuildState& bs, uint32_t runCount, const char*& finalPath) {
  const char* inPath = TIES_PATH_A;
  const char* outPath = TIES_PATH_B;

  while (runCount > 1) {
    auto inA = Storage.open(inPath);
    auto inB = Storage.open(inPath);
    auto out = Storage.open(outPath, O_RDWR | O_CREAT | O_TRUNC);
    if (!inA || !inB || !out) {
      LOG_ERR("FIDX", "tie merge pass open failed");
      return false;
    }

    const uint64_t inputSize = inA.fileSize64();
    uint64_t runStart = 0;
    uint32_t newRunCount = 0;
    bool ok = true;
    TieOutputBuffer output{&out, reinterpret_cast<uint8_t*>(bs.chunk.get())};

    for (uint32_t run = 0; ok && run < runCount; run += 2) {
      TieRunHeader headerA{};
      ok = inA.seek64(runStart) && inA.read(&headerA, sizeof(headerA)) == static_cast<int>(sizeof(headerA));
      const uint64_t bodyAStart = runStart + sizeof(TieRunHeader);
      if (!ok || !validateTieRun(headerA, bodyAStart, inputSize)) {
        if (!ok) LOG_ERR("FIDX", "tie run A seek/read failed");
        ok = false;
        break;
      }

      TieCursor cursorA{&inA, bs.nameA.get(), {}, headerA.count, headerA.bodyBytes};
      const uint64_t headerBStart = bodyAStart + headerA.bodyBytes;
      if (run + 1 >= runCount) {
        ok = writeBuffered(output, &headerA, sizeof(headerA));
        while (ok && cursorA.remaining > 0) {
          ok = loadTieRecord(cursorA) && writeTieRecord(output, cursorA);
          if (ok) consumeTieRecord(cursorA);
          maybeYield(bs.yieldCounter);
        }
        ok = ok && cursorA.bodyBytesRemaining == 0;
        runStart = headerBStart;
        newRunCount++;
        continue;
      }

      TieRunHeader headerB{};
      ok = inB.seek64(headerBStart) && inB.read(&headerB, sizeof(headerB)) == static_cast<int>(sizeof(headerB));
      const uint64_t bodyBStart = headerBStart + sizeof(TieRunHeader);
      if (!ok || !validateTieRun(headerB, bodyBStart, inputSize)) {
        if (!ok) LOG_ERR("FIDX", "tie run B seek/read failed");
        ok = false;
        break;
      }

      const uint64_t mergedCount = static_cast<uint64_t>(headerA.count) + headerB.count;
      const uint64_t mergedBytes = static_cast<uint64_t>(headerA.bodyBytes) + headerB.bodyBytes;
      if (mergedCount > UINT32_MAX || mergedBytes > UINT32_MAX) {
        LOG_ERR("FIDX", "tie run size overflow");
        ok = false;
        break;
      }

      const TieRunHeader mergedHeader{static_cast<uint32_t>(mergedCount), static_cast<uint32_t>(mergedBytes)};
      ok = writeBuffered(output, &mergedHeader, sizeof(mergedHeader));
      TieCursor cursorB{&inB, bs.nameB.get(), {}, headerB.count, headerB.bodyBytes};
      while (ok && (cursorA.remaining > 0 || cursorB.remaining > 0)) {
        ok = loadTieRecord(cursorA) && loadTieRecord(cursorB);
        if (!ok) break;

        bool takeA;
        if (cursorA.remaining > 0 && cursorB.remaining > 0) {
          const int cmp = FsHelpers::naturalCompare(cursorA.name, cursorB.name);
          takeA = cmp < 0 || (cmp == 0 && cursorA.record.blobOffset <= cursorB.record.blobOffset);
        } else {
          takeA = cursorA.remaining > 0;
        }

        TieCursor& selected = takeA ? cursorA : cursorB;
        ok = writeTieRecord(output, selected);
        if (ok) consumeTieRecord(selected);
        maybeYield(bs.yieldCounter);
      }
      ok = ok && cursorA.bodyBytesRemaining == 0 && cursorB.bodyBytesRemaining == 0;
      runStart = bodyBStart + headerB.bodyBytes;
      newRunCount++;
    }

    ok = ok && runStart == inputSize && flushBuffered(output);
    inA.close();
    inB.close();
    out.flush();
    out.close();
    if (!ok) {
      LOG_ERR("FIDX", "tie merge pass failed");
      return false;
    }

    std::swap(inPath, outPath);
    runCount = newRunCount;
  }

  finalPath = inPath;
  return true;
}

bool FileIndex::appendTieOffsets(BuildState& bs, const char* path, uint32_t recordCount, uint32_t& appendPos) {
  auto input = Storage.open(path);
  if (!input) {
    LOG_ERR("FIDX", "cannot open final tie run");
    return false;
  }

  const uint64_t inputSize = input.fileSize64();
  TieRunHeader header{};
  if (input.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) ||
      !validateTieRun(header, sizeof(TieRunHeader), inputSize) || header.count != recordCount ||
      sizeof(TieRunHeader) + header.bodyBytes != inputSize) {
    LOG_ERR("FIDX", "invalid final tie run");
    input.close();
    return false;
  }
  if (!bs.idxTmp.seek(appendPos)) {
    LOG_ERR("FIDX", "tie offset append seek failed");
    input.close();
    return false;
  }

  TieCursor cursor{&input, bs.nameA.get(), {}, header.count, header.bodyBytes};
  uint8_t* offsets = reinterpret_cast<uint8_t*>(bs.chunk.get());
  size_t used = 0;
  bool ok = true;
  while (ok && cursor.remaining > 0) {
    ok = loadTieRecord(cursor);
    if (!ok) break;
    if (used + sizeof(uint32_t) > CHUNK_BYTES) {
      ok = writeExact(bs.idxTmp, offsets, used, "tie offsets");
      used = 0;
    }
    if (ok) {
      memcpy(offsets + used, &cursor.record.blobOffset, sizeof(uint32_t));
      used += sizeof(uint32_t);
      consumeTieRecord(cursor);
    }
    maybeYield(bs.yieldCounter);
  }
  if (ok && used > 0) ok = writeExact(bs.idxTmp, offsets, used, "tie offsets");
  ok = ok && cursor.bodyBytesRemaining == 0;
  input.close();
  if (!ok) return false;

  appendPos += recordCount * sizeof(uint32_t);
  return true;
}

bool FileIndex::finishTieGroup(BuildState& bs, HalFile& tieOut, bool usesScratch, uint32_t groupCount,
                               uint32_t initialRunCount, bool hasPending, uint32_t pendingOffset, uint32_t& appendPos) {
  if (!usesScratch) return sortSmallTieGroup(bs, groupCount, appendPos);

  bool ok = true;
  if (hasPending) {
    ok = writeInitialTieRun(bs, tieOut, pendingOffset, false, 0, true);
    initialRunCount++;
  }
  if (initialRunCount != (groupCount + 1) / 2) {
    LOG_ERR("FIDX", "tie run count mismatch: runs=%u records=%u", initialRunCount, groupCount);
    ok = false;
  }
  tieOut.flush();
  tieOut.close();
  if (!ok) return false;

  const char* finalPath = nullptr;
  ok = mergeTieRuns(bs, initialRunCount, finalPath) && appendTieOffsets(bs, finalPath, groupCount, appendPos);
  if (ok) {
    Storage.remove(TIES_PATH_A);
    Storage.remove(TIES_PATH_B);
  }
  return ok;
}

bool FileIndex::writeOffsets(BuildState& bs, uint32_t recordCount) {
  auto run = Storage.open(bs.finalRunsPath);
  if (!run && recordCount > 0) {
    LOG_ERR("FIDX", "cannot open final run");
    return false;
  }

  uint32_t appendPos = bs.idxTmp.position();
  RunRecord groupKey{};
  uint32_t groupCount = 0;
  bool usesScratch = false;
  HalFile tieOut;
  uint32_t initialRunCount = 0;
  bool hasPending = false;
  uint32_t pendingOffset = 0;
  bool ok = true;

  for (uint32_t i = 0; ok && i < recordCount; i++) {
    RunRecord current{};
    if (run.read(&current, sizeof(current)) != static_cast<int>(sizeof(current))) {
      LOG_ERR("FIDX", "final run read failed");
      ok = false;
      break;
    }

    const bool sameKey = groupCount > 0 && memcmp(groupKey.key, current.key, sizeof(current.key)) == 0;
    if (!sameKey && groupCount > 0) {
      ok = finishTieGroup(bs, tieOut, usesScratch, groupCount, initialRunCount, hasPending, pendingOffset, appendPos);
      groupCount = 0;
      usesScratch = false;
      initialRunCount = 0;
      hasPending = false;
    }
    if (!ok) break;

    if (groupCount == 0) {
      groupKey = current;
      bs.chunk[0] = current;
      groupCount = 1;
    } else if (!usesScratch && groupCount < CHUNK_ENTRIES) {
      bs.chunk[groupCount++] = current;
    } else if (!usesScratch) {
      Storage.remove(TIES_PATH_A);
      Storage.remove(TIES_PATH_B);
      tieOut = Storage.open(TIES_PATH_A, O_RDWR | O_CREAT | O_TRUNC);
      if (!tieOut) {
        LOG_ERR("FIDX", "cannot open tie scratch");
        ok = false;
        break;
      }

      bs.idxTmp.flush();
      for (size_t pair = 0; ok && pair < CHUNK_ENTRIES; pair += 2) {
        ok = writeInitialTieRun(bs, tieOut, bs.chunk[pair].blobOffset, true, bs.chunk[pair + 1].blobOffset, false);
        initialRunCount++;
      }
      usesScratch = true;
      hasPending = true;
      pendingOffset = current.blobOffset;
      groupCount++;
    } else {
      if (hasPending) {
        ok = writeInitialTieRun(bs, tieOut, pendingOffset, true, current.blobOffset, true);
        if (ok) initialRunCount++;
        hasPending = false;
      } else {
        pendingOffset = current.blobOffset;
        hasPending = true;
      }
      groupCount++;
    }
    maybeYield(bs.yieldCounter);
  }

  if (ok && groupCount > 0) {
    ok = finishTieGroup(bs, tieOut, usesScratch, groupCount, initialRunCount, hasPending, pendingOffset, appendPos);
  }
  if (tieOut) tieOut.close();
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

bool FileIndex::pageNamesAt(size_t firstRow, size_t count, bool descending, char* names, size_t nameStride) {
  const size_t total = totalCount();
  if (!opened || names == nullptr || nameStride < 2 || firstRow >= total || count == 0) return false;

  count = std::min({count, MAX_PAGE_ENTRIES, total - firstRow});
  for (size_t i = 0; i < count; i++) {
    const size_t row = firstRow + i;
    size_t phys = 0;
    if (row < hdr.dirCount) {
      phys = descending ? (hdr.dirCount - 1 - row) : row;
    } else {
      const size_t fileRow = row - hdr.dirCount;
      phys = hdr.dirCount + (descending ? (hdr.fileCount - 1 - fileRow) : fileRow);
    }

    if (!readOffsetForPhysIndex(phys, pageRecordOffsets[i])) return false;
    pageOutputSlots[i] = static_cast<uint8_t>(i);
  }

  // The sorted view can be unrelated to directory enumeration order. Reorder
  // the reads physically, then put each name back into its display-order slot.
  for (size_t i = 1; i < count; i++) {
    const uint32_t candidateOffset = pageRecordOffsets[i];
    const uint8_t candidateSlot = pageOutputSlots[i];
    size_t pos = i;
    while (pos > 0 && pageRecordOffsets[pos - 1] > candidateOffset) {
      pageRecordOffsets[pos] = pageRecordOffsets[pos - 1];
      pageOutputSlots[pos] = pageOutputSlots[pos - 1];
      pos--;
    }
    pageRecordOffsets[pos] = candidateOffset;
    pageOutputSlots[pos] = candidateSlot;
  }

  for (size_t i = 0; i < count; i++) {
    const uint32_t recordOffset = pageRecordOffsets[i];
    if (idxFile.position() != recordOffset && !idxFile.seek(recordOffset)) return false;

    RecordHeader rec{};
    if (idxFile.read(&rec, sizeof(rec)) != static_cast<int>(sizeof(rec))) return false;

    const bool isDir = (rec.flags & 1) != 0;
    const size_t needed = static_cast<size_t>(rec.nameLen) + (isDir ? 1 : 0) + 1;
    if (needed > nameStride) return false;

    char* const out = names + static_cast<size_t>(pageOutputSlots[i]) * nameStride;
    if (idxFile.read(out, rec.nameLen) != static_cast<int>(rec.nameLen)) return false;
    size_t used = rec.nameLen;
    if (isDir) out[used++] = '/';
    out[used] = '\0';
  }
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
