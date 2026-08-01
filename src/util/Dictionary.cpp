#include "Dictionary.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "DictZip.h"
#include "DictionaryRegistry.h"
#include "StringUtils.h"

namespace {

// Shared temp file for entries lazily extracted from .dict.dz.
constexpr const char* DICT_TMP_FILE = "/.crosspoint/dict.tmp";

// Slack required above a definition buffer before we commit to reading it, so a
// request that would only just fit is refused rather than left to abort mid-read.
constexpr uint32_t DEFINITION_HEAP_HEADROOM_BYTES = 8 * 1024;

// Sampled-offset sidecar header, shared by the .qidx (over .idx) and .sidx
// (over .syn) sidecars: magic, version, sample interval, sample count, the
// source file size the sidecar was built from (staleness check), and the total
// entry count (bounds-checks the ordinal lookups a .syn hit resolves through).
constexpr uint32_t QIDX_MAGIC = 0x58444951;  // "QIDX" little-endian
constexpr uint32_t SIDX_MAGIC = 0x58444953;  // "SIDX" little-endian
constexpr uint32_t SIDECAR_VERSION = 2;      // bumped from 1: added entryCount
constexpr size_t SIDECAR_HEADER_BYTES = 6 * sizeof(uint32_t);

struct SidecarHeader {
  uint32_t sampleCount = 0;
  uint32_t sourceFileSize = 0;
  uint32_t entryCount = 0;
  bool valid = false;
};

SidecarHeader readSidecarHeader(HalFile& file, uint32_t magic, uint32_t sampleInterval) {
  SidecarHeader header;
  uint32_t raw[6];
  if (!file.seekSet(0) || file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) return header;
  if (raw[0] != magic || raw[1] != SIDECAR_VERSION || raw[2] != sampleInterval) return header;
  header.sampleCount = raw[3];
  header.sourceFileSize = raw[4];
  header.entryCount = raw[5];
  header.valid = true;
  return header;
}

bool readSampleOffset(HalFile& file, uint32_t sampleIndex, uint32_t* out) {
  if (!file.seekSet(SIDECAR_HEADER_BYTES + static_cast<size_t>(sampleIndex) * sizeof(uint32_t))) return false;
  return file.read(out, sizeof(*out)) == static_cast<int>(sizeof(*out));
}

uint32_t readBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Word characters for cleaning: ASCII alphanumerics plus any UTF-8
// continuation/lead byte, so accented words keep their edges.
bool isWordByte(unsigned char c) { return c >= 0x80 || std::isalnum(c) != 0; }

// True when the .ifo declares 64-bit index offsets, which this reader does not
// support (only scans the first 2KB — idxoffsetbits always appears early).
bool ifoDeclares64BitOffsets(const std::string& ifoPath) {
  HalFile ifo;
  if (!Storage.openFileForRead("DICT", ifoPath, ifo)) return false;
  char buf[2048];
  const int n = ifo.read(buf, sizeof(buf) - 1);
  if (n <= 0) return false;
  buf[n] = '\0';
  const char* line = strstr(buf, "idxoffsetbits");
  if (!line) return false;
  const char* eq = strchr(line, '=');
  return eq && strtol(eq + 1, nullptr, 10) == 64;
}

}  // namespace

bool Dictionary::open(const char* folderName) {
  basePath.clear();
  hasSyn = false;
  std::string resolved;
  if (!DictionaryRegistry::resolveBasePath(folderName, resolved)) {
    LOG_ERR("DICT", "No dictionary found in folder '%s'", folderName ? folderName : "");
    return false;
  }

  if (!Storage.exists((resolved + ".idx").c_str())) {
    LOG_ERR("DICT", "%s.idx missing (compressed .idx.gz is not supported)", resolved.c_str());
    return false;
  }
  hasPlainDict = Storage.exists((resolved + ".dict").c_str());
  if (!hasPlainDict && !Storage.exists((resolved + ".dict.dz").c_str())) {
    LOG_ERR("DICT", "%s has no .dict or .dict.dz", resolved.c_str());
    return false;
  }
  if (ifoDeclares64BitOffsets(resolved + ".ifo")) {
    LOG_ERR("DICT", "%s uses 64-bit index offsets (unsupported)", resolved.c_str());
    return false;
  }
  hasSyn = Storage.exists((resolved + ".syn").c_str());

  // Checked once here so buildPath() can never fail on the lookup path.
  if (resolved.size() + LONGEST_SUFFIX_LEN + 1 > PATH_BUF_BYTES) {
    LOG_ERR("DICT", "Dictionary path too long (%u chars, max %u)", static_cast<unsigned>(resolved.size()),
            static_cast<unsigned>(PATH_BUF_BYTES - LONGEST_SUFFIX_LEN - 1));
    return false;
  }

  basePath = std::move(resolved);
  return true;
}

bool Dictionary::buildPath(char* buf, size_t bufSize, const char* suffix) const {
  const int n = snprintf(buf, bufSize, "%s%s", basePath.c_str(), suffix);
  if (n < 0 || static_cast<size_t>(n) >= bufSize) {
    LOG_ERR("DICT", "Path too long: %s%s", basePath.c_str(), suffix);
    return false;
  }
  return true;
}

// A sidecar is stale when it is missing, unreadable, the wrong version, or built
// from a different source size. Missing source returns false (not stale): the
// dictionary is unusable without its .idx, and a vanished .syn degrades to no
// synonyms — neither is fixable by re-indexing here.
bool Dictionary::sidecarIsStale(const std::string& sourcePath, const std::string& sidecarPath, uint32_t magic) {
  HalFile src;
  if (!Storage.openFileForRead("DICT", sourcePath, src)) return false;
  const uint32_t srcSize = static_cast<uint32_t>(src.fileSize());

  HalFile sidecar;
  if (!Storage.openFileForRead("DICT", sidecarPath, sidecar)) return true;
  const SidecarHeader header = readSidecarHeader(sidecar, magic, SAMPLE_INTERVAL);
  return !header.valid || header.sourceFileSize != srcSize;
}

bool Dictionary::needsIndex() {
  if (!isOpen()) return false;
  if (sidecarIsStale(basePath + ".idx", basePath + ".qidx", QIDX_MAGIC)) return true;
  return hasSyn && sidecarIsStale(basePath + ".syn", basePath + ".sidx", SIDX_MAGIC);
}

bool Dictionary::buildIndex(void (*yieldFn)(void*), void* ctx, IndexResult* outResult) {
  if (outResult) *outResult = IndexResult::Ok;
  if (!isOpen()) {
    if (outResult) *outResult = IndexResult::ReadError;
    return false;
  }

  // The .idx sidecar is mandatory — lookups binary-search it. Rebuild only when
  // stale so a .syn-only change doesn't force a needless rescan of the (much
  // larger) .idx, and vice versa.
  if (sidecarIsStale(basePath + ".idx", basePath + ".qidx", QIDX_MAGIC) &&
      !buildSidecar(basePath + ".idx", basePath + ".qidx", QIDX_MAGIC, 8, yieldFn, ctx, outResult)) {
    return false;
  }

  // The synonym sidecar is best-effort: a failure here (e.g. transient OOM)
  // leaves synonym lookups disabled but the dictionary otherwise usable. Clear
  // hasSyn so this session's lookups skip locateSynonym() rather than falling
  // back to a full linear .syn scan on every miss; needsIndex() still reports it
  // stale, so the next open() retries the build. Not reported through
  // *outResult — only the mandatory .idx build failure is fatal to the caller.
  if (hasSyn && sidecarIsStale(basePath + ".syn", basePath + ".sidx", SIDX_MAGIC) &&
      !buildSidecar(basePath + ".syn", basePath + ".sidx", SIDX_MAGIC, 4, yieldFn, ctx, nullptr)) {
    LOG_ERR("DICT", "Synonym index build failed; synonyms disabled for %s", basePath.c_str());
    hasSyn = false;
  }
  return true;
}

bool Dictionary::buildSidecar(const std::string& sourcePath, const std::string& sidecarPath, uint32_t magic,
                              uint32_t suffixBytes, void (*yieldFn)(void*), void* ctx, IndexResult* outResult) {
  const auto fail = [outResult](IndexResult r) {
    if (outResult) *outResult = r;
    return false;
  };

  HalFile src;
  if (!Storage.openFileForRead("DICT", sourcePath, src)) return fail(IndexResult::ReadError);
  const uint32_t srcSize = static_cast<uint32_t>(src.fileSize());

  constexpr size_t CHUNK_BYTES = 4096;
  auto buf = makeUniqueNoThrow<uint8_t[]>(CHUNK_BYTES);
  if (!buf) {
    LOG_ERR("DICT", "OOM: %u byte index scan buffer", CHUNK_BYTES);
    return fail(IndexResult::LowMemory);
  }

  // Stream each sample offset straight to the sidecar instead of accumulating
  // them in RAM: a large source would otherwise cost tens of KB of vector heap,
  // and vector growth aborts on OOM under -fno-exceptions. The header slot is
  // zero-filled until the scan succeeds, so an interrupted build leaves a file
  // readSidecarHeader rejects (magic mismatch) and needsIndex() triggers a rebuild.
  HalFile out;
  if (!Storage.openFileForWrite("DICT", sidecarPath, out)) return fail(IndexResult::ReadError);
  const auto writeU32 = [&out](uint32_t v) { return out.write(&v, sizeof(v)) == static_cast<int>(sizeof(v)); };
  const uint32_t placeholder[6] = {};
  bool ok = out.write(placeholder, sizeof(placeholder)) == sizeof(placeholder);
  uint32_t sampleCount = 0;
  if (ok) {
    ok = writeU32(0);  // entry 0 always starts at byte 0
    sampleCount = 1;
  }

  const unsigned long startMs = millis();
  uint32_t entryCount = 0;
  uint32_t pos = 0;
  uint32_t suffixLeft = 0;  // 0 while scanning a word, else suffix bytes remaining
  uint32_t sinceYield = 0;
  while (ok && pos < srcSize) {
    const int n = src.read(buf.get(), CHUNK_BYTES);
    if (n <= 0) {
      LOG_ERR("DICT", "Index scan read failed at %lu", static_cast<unsigned long>(pos));
      ok = false;
      break;
    }
    for (int i = 0; ok && i < n; i++) {
      if (suffixLeft == 0) {
        if (buf[i] == 0) suffixLeft = suffixBytes;
      } else if (--suffixLeft == 0) {
        entryCount++;
        const uint32_t nextEntryStart = pos + i + 1;
        if (entryCount % SAMPLE_INTERVAL == 0 && nextEntryStart < srcSize) {
          ok = writeU32(nextEntryStart);
          sampleCount++;
        }
      }
    }
    pos += n;
    sinceYield += n;
    if (yieldFn && sinceYield >= 64 * 1024) {
      sinceYield = 0;
      yieldFn(ctx);
    }
  }

  if (ok) {
    // Backpatch the now-valid header over the placeholder.
    const uint32_t header[6] = {magic, SIDECAR_VERSION, SAMPLE_INTERVAL, sampleCount, srcSize, entryCount};
    ok = out.seekSet(0) && out.write(header, sizeof(header)) == sizeof(header);
  }
  if (!ok) {
    LOG_ERR("DICT", "Index build failed, removing %s", sidecarPath.c_str());
    out.close();  // close before remove of the same path
    Storage.remove(sidecarPath.c_str());
    return fail(IndexResult::ReadError);
  }

  LOG_INF("DICT", "Indexed %lu entries (%lu samples) from %s in %lu ms", static_cast<unsigned long>(entryCount),
          static_cast<unsigned long>(sampleCount), sourcePath.c_str(), millis() - startMs);
  return true;
}

int Dictionary::readWordInto(HalFile& file, char* buf, size_t bufSize) {
  size_t i = 0;
  while (i < bufSize - 1) {
    const int ch = file.read();
    if (ch < 0) return -1;  // EOF or I/O error
    if (ch == 0) {
      buf[i] = '\0';
      return static_cast<int>(i);
    }
    buf[i++] = static_cast<char>(ch);
  }
  // Word too long for buffer — consume remaining bytes to stay in sync
  buf[bufSize - 1] = '\0';
  int ch;
  do {
    ch = file.read();
  } while (ch > 0);
  return static_cast<int>(bufSize - 1);
}

bool Dictionary::openSession(LookupSession& session) {
  if (!isOpen()) return false;

  // One buffer reused for both paths — no transient heap on the lookup path.
  char path[PATH_BUF_BYTES];
  if (!buildPath(path, sizeof(path), ".idx")) return false;
  if (!Storage.openFileForRead("DICT", path, session.idx)) return false;
  session.idxSize = static_cast<uint32_t>(session.idx.fileSize());

  // The sidecar is optional and its header only has to be validated once per
  // lookup; without a usable one locate() scans from byte 0.
  if (!buildPath(path, sizeof(path), ".qidx")) return true;
  if (Storage.openFileForRead("DICT", path, session.qidx)) {
    const SidecarHeader header = readSidecarHeader(session.qidx, QIDX_MAGIC, SAMPLE_INTERVAL);
    if (header.valid && header.sourceFileSize == session.idxSize) session.sampleCount = header.sampleCount;
  }
  return true;
}

// Both files stay open across the stem-variant probes; every read below seeks
// absolutely first, so a shared handle carries no position state between calls.
DictLocation Dictionary::locate(LookupSession& session, const char* target, std::string* matchedHeadwordOut) {
  DictLocation result;

  // Bisect the sampled offsets to the last sample whose headword <= target.
  // Falls back to a full scan from byte 0 when the sidecar is unusable.
  uint32_t startByte = 0;
  if (session.sampleCount > 0) {
    uint32_t lo = 0;
    uint32_t hi = session.sampleCount - 1;
    while (lo < hi) {
      const uint32_t mid = (lo + hi + 1) / 2;
      uint32_t offset = 0;
      if (!readSampleOffset(session.qidx, mid, &offset) || !session.idx.seekSet(offset) ||
          readWordInto(session.idx, wordBuf, sizeof(wordBuf)) < 0) {
        lo = 0;
        break;
      }
      if (StringUtils::asciiCaseCmp(wordBuf, target) <= 0) {
        lo = mid;
      } else {
        hi = mid - 1;
      }
    }
    readSampleOffset(session.qidx, lo, &startByte);
  }

  // Linear scan of at most SAMPLE_INTERVAL entries: headword NUL, BE32 offset,
  // BE32 size. The index is sorted, so stop at the first headword > target.
  if (!session.idx.seekSet(startByte)) {
    LOG_ERR("DICT", "Index seek to %lu failed", static_cast<unsigned long>(startByte));
    result.readError = true;
    return result;
  }
  while (static_cast<uint32_t>(session.idx.position()) < session.idxSize) {
    // Not flagged as readError: readWordInto returns -1 for EOF and IO error
    // alike, so a short tail can't be told from a truncated .idx. Treat it as
    // the end of the index rather than risk reporting a read failure for what
    // is really a miss.
    if (readWordInto(session.idx, wordBuf, sizeof(wordBuf)) < 0) break;
    uint8_t suffix[8];
    if (session.idx.read(suffix, 8) != 8) break;

    const int cmp = StringUtils::asciiCaseCmp(wordBuf, target);
    if (cmp == 0) {
      result.offset = readBe32(suffix);
      result.size = readBe32(suffix + 4);
      result.found = true;
      if (matchedHeadwordOut) *matchedHeadwordOut = wordBuf;
      return result;
    }
    if (cmp > 0) break;
  }
  return result;
}

DictLocation Dictionary::locateByOrdinal(uint32_t ordinal, std::string* matchedHeadwordOut) {
  DictLocation result;
  if (!isOpen()) return result;

  HalFile idx;
  if (!Storage.openFileForRead("DICT", basePath + ".idx", idx)) {
    result.readError = true;
    return result;
  }
  const uint32_t idxSize = static_cast<uint32_t>(idx.fileSize());

  // The .qidx samples are taken at fixed entry-count boundaries (every
  // SAMPLE_INTERVAL entries), so sample (ordinal / SAMPLE_INTERVAL) lands on
  // entry (ordinal / SAMPLE_INTERVAL) * SAMPLE_INTERVAL; scan forward the
  // remainder. Without a usable sidecar, count entries from byte 0.
  uint32_t startByte = 0;
  uint32_t startOrdinal = 0;
  HalFile qidx;
  if (Storage.openFileForRead("DICT", basePath + ".qidx", qidx)) {
    const SidecarHeader header = readSidecarHeader(qidx, QIDX_MAGIC, SAMPLE_INTERVAL);
    if (header.valid && header.sourceFileSize == idxSize && header.sampleCount > 0) {
      if (header.entryCount != 0 && ordinal >= header.entryCount) return result;  // out of range
      uint32_t sampleIndex = ordinal / SAMPLE_INTERVAL;
      if (sampleIndex >= header.sampleCount) sampleIndex = header.sampleCount - 1;
      if (readSampleOffset(qidx, sampleIndex, &startByte)) {
        startOrdinal = sampleIndex * SAMPLE_INTERVAL;
      } else {
        startByte = 0;
      }
    }
  }

  // Each entry is headword NUL + BE32 offset + BE32 size. Skip to the target,
  // guarding against EOF for a malformed .syn ordinal past the last entry.
  if (!idx.seekSet(startByte)) {
    result.readError = true;
    return result;
  }
  uint8_t suffix[8];
  for (uint32_t e = startOrdinal; e < ordinal; e++) {
    if (readWordInto(idx, wordBuf, sizeof(wordBuf)) < 0 || idx.read(suffix, 8) != 8) return result;
    if (static_cast<uint32_t>(idx.position()) >= idxSize) return result;  // ordinal past last entry
  }
  if (static_cast<uint32_t>(idx.position()) >= idxSize) return result;
  if (readWordInto(idx, wordBuf, sizeof(wordBuf)) < 0 || idx.read(suffix, 8) != 8) return result;
  result.offset = readBe32(suffix);
  result.size = readBe32(suffix + 4);
  result.found = true;
  if (matchedHeadwordOut) *matchedHeadwordOut = wordBuf;
  return result;
}

DictLocation Dictionary::locateSynonym(const char* target, std::string* matchedHeadwordOut) {
  DictLocation result;
  if (!isOpen() || !hasSyn) return result;

  HalFile syn;
  if (!Storage.openFileForRead("DICT", basePath + ".syn", syn)) {
    result.readError = true;
    return result;
  }
  const uint32_t synSize = static_cast<uint32_t>(syn.fileSize());

  // Bisect the sampled offsets to the last synonym <= target, mirroring
  // locate(). Falls back to a full scan from byte 0 when the sidecar is unusable.
  uint32_t startByte = 0;
  HalFile sidx;
  if (Storage.openFileForRead("DICT", basePath + ".sidx", sidx)) {
    const SidecarHeader header = readSidecarHeader(sidx, SIDX_MAGIC, SAMPLE_INTERVAL);
    if (header.valid && header.sourceFileSize == synSize && header.sampleCount > 0) {
      uint32_t lo = 0;
      uint32_t hi = header.sampleCount - 1;
      while (lo < hi) {
        const uint32_t mid = (lo + hi + 1) / 2;
        uint32_t offset = 0;
        if (!readSampleOffset(sidx, mid, &offset) || !syn.seekSet(offset) ||
            readWordInto(syn, wordBuf, sizeof(wordBuf)) < 0) {
          lo = 0;
          break;
        }
        if (StringUtils::asciiCaseCmp(wordBuf, target) <= 0) {
          lo = mid;
        } else {
          hi = mid - 1;
        }
      }
      readSampleOffset(sidx, lo, &startByte);
    }
  }

  // Linear scan of at most SAMPLE_INTERVAL entries: synonym NUL, BE32 ordinal.
  // Sorted, so stop at the first synonym > target. Reading the ordinal before
  // calling locateByOrdinal() (which reuses wordBuf) avoids any aliasing.
  if (!syn.seekSet(startByte)) {
    result.readError = true;
    return result;
  }
  while (static_cast<uint32_t>(syn.position()) < synSize) {
    if (readWordInto(syn, wordBuf, sizeof(wordBuf)) < 0) break;
    uint8_t ordBytes[4];
    if (syn.read(ordBytes, 4) != 4) break;

    const int cmp = StringUtils::asciiCaseCmp(wordBuf, target);
    if (cmp == 0) return locateByOrdinal(readBe32(ordBytes), matchedHeadwordOut);
    if (cmp > 0) break;
  }
  return result;
}

bool Dictionary::readDefinition(const DictLocation& location, std::string& out, LookupResult* outResult) {
  const auto fail = [outResult](LookupResult r) {
    if (outResult) *outResult = r;
    return false;
  };
  if (!location.found) return fail(LookupResult::NotFound);
  const uint32_t size = std::min(location.size, MAX_DEFINITION_BYTES);
  if (size == 0) {
    LOG_ERR("DICT", "Zero-length definition entry");
    return fail(LookupResult::ReadError);
  }

  // Refuse before touching the heap or the SD: std::string growth aborts on OOM
  // (-fno-exceptions), and the extraction below transiently holds a chunk buffer
  // plus a 32KB inflate window we'd rather not commit to a doomed lookup.
  if (ESP.getMaxAllocHeap() < size + DEFINITION_HEAP_HEADROOM_BYTES) {
    LOG_ERR("DICT", "Low heap for %lu byte definition", static_cast<unsigned long>(size));
    return fail(LookupResult::LowMemory);
  }

  char pathBuf[PATH_BUF_BYTES];
  const char* path = pathBuf;
  uint32_t offset = 0;
  if (hasPlainDict) {
    if (!buildPath(pathBuf, sizeof(pathBuf), ".dict")) return fail(LookupResult::ReadError);
    offset = location.offset;
  } else {
    if (!buildPath(pathBuf, sizeof(pathBuf), ".dict.dz")) return fail(LookupResult::ReadError);
    HalFile tmp = Storage.open(DICT_TMP_FILE, O_WRITE | O_CREAT | O_TRUNC);
    if (!tmp) {
      LOG_ERR("DICT", "Failed to open %s", DICT_TMP_FILE);
      return fail(LookupResult::ReadError);
    }
    DictZip::ExtractError xerr = DictZip::ExtractError::None;
    if (!DictZip::extractEntry(pathBuf, location.offset, size, tmp, &xerr)) {
      // Map the specific extraction cause to the lookup result: allocation
      // failure (heap fragmentation) vs corrupt/truncated .dz vs an IO error.
      LOG_ERR("DICT", "dictzip extraction failed for %s (error %d)", basePath.c_str(), static_cast<int>(xerr));
      switch (xerr) {
        case DictZip::ExtractError::LowMemory:
          return fail(LookupResult::LowMemory);
        case DictZip::ExtractError::ReadError:
          return fail(LookupResult::ReadError);
        case DictZip::ExtractError::Decompress:
        default:
          return fail(LookupResult::Decompress);
      }
    }
    tmp.close();  // close before reopening the same path for read
    path = DICT_TMP_FILE;
  }

  HalFile dict;
  if (!Storage.openFileForRead("DICT", path, dict)) return fail(LookupResult::ReadError);
  const uint32_t dictSize = static_cast<uint32_t>(dict.fileSize());
  if (offset > dictSize || size > dictSize - offset) {
    LOG_ERR("DICT", "Definition out of bounds (%lu+%lu > %lu)", static_cast<unsigned long>(offset),
            static_cast<unsigned long>(size), static_cast<unsigned long>(dictSize));
    return fail(LookupResult::ReadError);
  }

  if (!dict.seekSet(offset)) {
    LOG_ERR("DICT", "Seek to %lu failed", static_cast<unsigned long>(offset));
    return fail(LookupResult::ReadError);
  }
  out.assign(size, '\0');
  // The bounds check above guarantees the bytes exist, so a short read is an IO
  // failure — surface it instead of returning a silently truncated definition.
  if (dict.read(&out[0], size) != static_cast<int>(size)) {
    out.clear();
    return fail(LookupResult::ReadError);
  }
  return true;
}

std::string Dictionary::cleanWord(const char* word) {
  if (!word) return "";
  size_t start = 0;
  size_t end = strlen(word);
  while (start < end && !isWordByte(static_cast<unsigned char>(word[start]))) start++;
  while (end > start && !isWordByte(static_cast<unsigned char>(word[end - 1]))) end--;
  if (start >= end) return "";

  std::string result(word + start, end - start);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return c >= 0x80 ? c : static_cast<unsigned char>(std::tolower(c)); });
  return result;
}

void Dictionary::stemVariants(const std::string& word, std::vector<std::string>& out) {
  out.clear();
  out.reserve(6);
  const size_t n = word.size();
  const auto add = [&out](std::string v) {
    if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(std::move(v));
  };
  // endsWith requires a non-empty remainder so variants never come out empty.
  const auto endsWith = [&word, n](const char* suffix) {
    const size_t len = strlen(suffix);
    return n > len && word.compare(n - len, len, suffix) == 0;
  };

  if (endsWith("'s")) add(word.substr(0, n - 2));
  if (endsWith("\xE2\x80\x99s")) add(word.substr(0, n - 4));  // U+2019 apostrophe
  if (endsWith("ies")) add(word.substr(0, n - 3) + "y");      // stories -> story
  if (endsWith("es")) add(word.substr(0, n - 2));             // boxes -> box
  if (endsWith("s")) add(word.substr(0, n - 1));              // dogs -> dog
  if (endsWith("ed")) {
    add(word.substr(0, n - 2));                                            // walked -> walk
    add(word.substr(0, n - 1));                                            // loved -> love
    if (n >= 4 && word[n - 3] == word[n - 4]) add(word.substr(0, n - 3));  // stopped -> stop
  }
  if (endsWith("ing")) {
    add(word.substr(0, n - 3));                                            // walking -> walk
    add(word.substr(0, n - 3) + "e");                                      // making -> make
    if (n >= 5 && word[n - 4] == word[n - 5]) add(word.substr(0, n - 4));  // running -> run
  }
}

bool Dictionary::lookup(const char* word, std::string& definitionOut, std::string& matchedHeadwordOut,
                        LookupResult* outResult) {
  const auto setResult = [outResult](LookupResult r) {
    if (outResult) *outResult = r;
  };
  setResult(LookupResult::NotFound);
  const std::string cleaned = cleanWord(word);
  if (cleaned.empty() || !isOpen()) return false;

  // One set of open handles for the exact-match probe and every stem variant,
  // scoped so .idx/.qidx close before readDefinition() opens the data file.
  DictLocation location;
  bool searchFailed = false;
  {
    LookupSession session;
    // Couldn't open .idx: the search never reached a verdict, so this is a read
    // failure, not a miss.
    if (!openSession(session)) {
      setResult(LookupResult::ReadError);
      return false;
    }

    location = locate(session, cleaned.c_str(), &matchedHeadwordOut);
    searchFailed = location.readError;
    // Dictionary-authored synonyms (alternate spellings, irregular forms) take
    // precedence over the English-only stemmer, and are language-agnostic.
    if (!location.found && hasSyn) {
      location = locateSynonym(cleaned.c_str(), &matchedHeadwordOut);
      searchFailed = searchFailed || location.readError;
    }
    if (!location.found) {
      std::vector<std::string> variants;
      stemVariants(cleaned, variants);
      for (const auto& variant : variants) {
        location = locate(session, variant.c_str(), &matchedHeadwordOut);
        searchFailed = searchFailed || location.readError;
        if (location.found) break;
      }
    }
  }
  if (!location.found) {
    // A search that never reached a verdict (couldn't open or seek .idx/.syn) is
    // a read failure, not a miss — reporting "Not found" would be wrong.
    // Otherwise the word is genuinely not in the dictionary.
    if (searchFailed) setResult(LookupResult::ReadError);
    return false;
  }

  // Found in the index — propagate the precise failure reason from readDefinition
  // (decompression / low memory / read error) so the caller can name it.
  if (readDefinition(location, definitionOut, outResult)) {
    setResult(LookupResult::Found);
    return true;
  }
  return false;
}
