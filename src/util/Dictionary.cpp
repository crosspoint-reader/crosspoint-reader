#include "Dictionary.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "DictZip.h"
#include "DictionaryRegistry.h"
#include "StringUtils.h"

namespace {

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

// Punctuation, symbols and spaces beyond ASCII that cleanWord() strips from a
// word's edges. isWordByte() keeps every byte >= 0x80 so accented letters
// survive, which also left «Über», ¿Qué, （词） and 「言葉」 in their marks.
//
// Based on crosspoint-reader PR #3611 (Anton Lijcklama à Nijeholt,
// @alycklama), reworked as a short list of the marks books put against a word
// instead of Unicode's category data (~6 KB of tables there).
struct CodepointRange {
  uint16_t first;
  uint16_t last;
};
constexpr CodepointRange EDGE_PUNCTUATION[] = {
    {0x00A0, 0x00A9},  // no-break space ¡ ¢ £ ¤ ¥ ¦ § ¨ ©
    {0x00AB, 0x00B4},  // « ¬ soft hyphen ® ¯ ° ± ² ³ ´
    {0x00B6, 0x00B9},  // ¶ · ¸ ¹
    {0x00BB, 0x00BF},  // » ¼ ½ ¾ ¿ (ª µ º between them are letters)
    {0x037E, 0x037E},  // Greek question mark
    {0x0387, 0x0387},  // Greek ano teleia
    {0x2000, 0x207F},  // General Punctuation (quotes, dashes, ellipsis, ‹ ›) and superscripts
    {0x3000, 0x3004},  // ideographic space 、 。 〃 〄
    {0x3008, 0x3020},  // CJK brackets and quotes 〈 〉 《 》 「 」 『 』 【 】 〜 〝 〞
    {0xFF01, 0xFF0F},  // fullwidth ！ ＂ ＃ ＄ ％ ＆ ＇ （ ） ＊ ＋ ， － ． ／
    {0xFF1A, 0xFF20},  // fullwidth ： ； ＜ ＝ ＞ ？ ＠
    {0xFF3B, 0xFF40},  // fullwidth ［ ＼ ］ ＾ ＿ ｀
    {0xFF5B, 0xFF65},  // fullwidth ｛ ｜ ｝ ～ ｟ ｠, halfwidth ｡ ｢ ｣ ､ ･
};

// Byte length of the strippable non-ASCII codepoint that starts at b[i] and
// ends at or before b[end], or 0 when there is none there. b is NUL-terminated.
size_t edgePunctuationLen(const unsigned char* b, size_t i, size_t end) {
  if (b[i] < 0x80) return 0;
  const unsigned char* p = b + i;
  const uint32_t cp = utf8NextCodepoint(&p);
  const auto len = static_cast<size_t>(p - (b + i));
  if (i + len > end) return 0;
  if (utf8IsDefaultIgnorable(cp)) return len;
  for (const auto& range : EDGE_PUNCTUATION) {
    if (cp >= range.first && cp <= range.last) return len;
  }
  return 0;
}

// Headwords equal under asciiCaseCmp sit in one contiguous run, ordered among
// themselves by StarDict's strcmp tiebreak -- which puts "Husk" (a surname)
// ahead of "husk", since uppercase sorts first in ASCII. Taking the first of
// the run answered a lookup of "husk" with the proper noun. So the scan keeps
// the first entry of the run as a fallback (a query for "paris" against a
// dictionary holding only "Paris") but moves on to the entry spelled exactly
// like the target when the run has one. True when `word` becomes the pick;
// `exact` tells the caller the run needs no further scanning. Shared by the
// .idx and .syn scans, which have the same runs.
//
// Ported from crosspoint-reader PR #3480 (Lennon Anderson, @YurBoiRene); the
// same fix is PR #3436 (@PrashikshitSaini). Upstream fixed the .idx scan only.
bool takesRunEntry(const char* word, const char* target, bool havePick, bool& exact) {
  exact = strcmp(word, target) == 0;
  return exact || !havePick;
}

// Facts read from the .ifo at open time. Only the first 2KB is scanned — .ifo
// headers are tiny and both keys always appear early when present.
struct IfoFacts {
  bool offsets64 = false;        // idxoffsetbits=64 (unsupported)
  bool htmlDefinitions = false;  // sametypesequence=h (definitions are HTML)
};

IfoFacts readIfoFacts(const std::string& ifoPath) {
  IfoFacts facts;
  HalFile ifo;
  if (!Storage.openFileForRead("DICT", ifoPath, ifo)) return facts;
  char buf[2048];
  const int n = ifo.read(buf, sizeof(buf) - 1);
  if (n <= 0) return facts;
  buf[n] = '\0';
  const char* line = strstr(buf, "idxoffsetbits");
  const char* eq = line ? strchr(line, '=') : nullptr;
  facts.offsets64 = eq && strtol(eq + 1, nullptr, 10) == 64;
  line = strstr(buf, "sametypesequence");
  eq = line ? strchr(line, '=') : nullptr;
  if (eq) {
    // Only the single-field sequence "h" is treated as HTML; multi-type
    // entries keep the plain-text viewing path.
    facts.htmlDefinitions = eq[1] == 'h' && (eq[2] == '\0' || eq[2] == '\r' || eq[2] == '\n');
  }
  return facts;
}

}  // namespace

// --- ScanReader -------------------------------------------------------------

void Dictionary::ScanReader::attach(HalFile* f, uint8_t* b, uint32_t size) {
  file = f;
  buf = b;
  fileSize = size;
  chunkStart = 0;
  filled = 0;
  cursor = 0;
  posDirty = true;
}

bool Dictionary::ScanReader::seekSet(uint32_t offset) {
  if (!file || offset > fileSize) return false;
  // Stay inside the buffer when the target is already resident — the bisect's
  // final bound reads and locate()'s seek to the window start often land here.
  if (filled > 0 && offset >= chunkStart && offset < chunkStart + static_cast<uint32_t>(filled)) {
    cursor = static_cast<int>(offset - chunkStart);
    return true;
  }
  chunkStart = offset;
  filled = 0;
  cursor = 0;
  posDirty = true;
  return true;
}

bool Dictionary::ScanReader::refill() {
  if (!file || !buf) return false;
  const uint32_t nextOff = chunkStart + static_cast<uint32_t>(filled);
  if (nextOff >= fileSize) return false;
  // The handle is ours alone, so after a read its position is already nextOff;
  // only an explicit seekSet() makes a seek necessary.
  if (posDirty && !file->seekSet(nextOff)) return false;
  const int n = file->read(buf, SCAN_BUF_BYTES);
  if (n <= 0) {
    posDirty = true;
    return false;
  }
  chunkStart = nextOff;
  filled = n;
  cursor = 0;
  posDirty = false;
  return true;
}

int Dictionary::ScanReader::readWord(char* out, size_t outSize) {
  if (!out || outSize == 0) return -1;
  size_t written = 0;
  for (;;) {
    if (cursor >= filled && !refill()) {
      // EOF or IO error, and a word left unterminated by the end of the file is
      // a truncated index -- reported the same way, because the caller's only
      // sensible response to either is to stop scanning.
      return -1;
    }
    const uint8_t* start = buf + cursor;
    const size_t avail = static_cast<size_t>(filled - cursor);
    const auto* nul = static_cast<const uint8_t*>(memchr(start, 0, avail));
    const size_t take = nul ? static_cast<size_t>(nul - start) : avail;
    if (written < outSize - 1) {
      const size_t copy = std::min(take, outSize - 1 - written);
      memcpy(out + written, start, copy);
      written += copy;
    }
    cursor += static_cast<int>(take);
    if (nul) {
      cursor++;  // step over the NUL so the stream stays in sync
      out[written] = '\0';
      return static_cast<int>(written);
    }
  }
}

bool Dictionary::ScanReader::readBytes(void* dst, size_t n) {
  auto* p = static_cast<uint8_t*>(dst);
  while (n > 0) {
    if (cursor >= filled && !refill()) return false;
    const size_t take = std::min(n, static_cast<size_t>(filled - cursor));
    memcpy(p, buf + cursor, take);
    p += take;
    cursor += static_cast<int>(take);
    n -= take;
  }
  return true;
}

// --- Dictionary -------------------------------------------------------------

bool Dictionary::open(const char* folderName) {
  basePath.clear();
  hasSyn = false;
  htmlDefinitions = false;
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
  const IfoFacts ifo = readIfoFacts(resolved + ".ifo");
  if (ifo.offsets64) {
    LOG_ERR("DICT", "%s uses 64-bit index offsets (unsupported)", resolved.c_str());
    return false;
  }
  // Checked once here so buildPath() can never fail on the lookup path.
  if (resolved.size() + LONGEST_SUFFIX_LEN + 1 > PATH_BUF_BYTES) {
    LOG_ERR("DICT", "Dictionary path too long (%u chars, max %u)", static_cast<unsigned>(resolved.size()),
            static_cast<unsigned>(PATH_BUF_BYTES - LONGEST_SUFFIX_LEN - 1));
    return false;
  }
  hasSyn = Storage.exists((resolved + ".syn").c_str());
  htmlDefinitions = ifo.htmlDefinitions;

  // A previously open dictionary's cached chunk table describes a different
  // file; the buffers themselves are content-free and stay.
  dzScratch.release();

  basePath = std::move(resolved);
  return true;
}

void Dictionary::releaseCaches() {
  scanBuf.reset();
  dzScratch.release();
}

bool Dictionary::ensureScanBuffers() {
  if (scanBuf) return true;
  scanBuf = makeUniqueNoThrow<ScanBuffers>();
  if (!scanBuf) {
    LOG_ERR("DICT", "OOM: %u byte scan buffers", static_cast<unsigned>(sizeof(ScanBuffers)));
    return false;
  }
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
// synonyms — neither is fixable by re-indexing here. This is the same rule
// openSession() applies to .qidx (a successful build always writes at least
// entry 0, so sampleCount == 0 means absent, stale or corrupt), expressed over a
// path so it can cover the .syn/.sidx pair too.
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
  const auto fail = [outResult](IndexResult r) {
    if (outResult) *outResult = r;
    return false;
  };
  if (outResult) *outResult = IndexResult::Ok;
  if (!isOpen()) return fail(IndexResult::ReadError);

  // The .idx sidecar is mandatory — lookups binary-search it. Rebuild only when
  // stale so a .syn-only change doesn't force a needless rescan of the (much
  // larger) .idx, and vice versa.
  if (sidecarIsStale(basePath + ".idx", basePath + ".qidx", QIDX_MAGIC) &&
      !buildSidecar(basePath + ".idx", basePath + ".qidx", QIDX_MAGIC, 8, yieldFn, ctx, outResult)) {
    return false;
  }

  // The synonym sidecar is best-effort: a failure here (e.g. transient OOM)
  // leaves synonym lookups disabled but the dictionary otherwise usable, so it
  // does not fail the build or overwrite *outResult. hasSyn is left alone — it
  // means "a .syn file exists", so needsIndex() keeps reporting the sidecar
  // stale and a later build retries. openSynonyms() is what declines the
  // synonym path while the sidecar is unusable.
  if (hasSyn && sidecarIsStale(basePath + ".syn", basePath + ".sidx", SIDX_MAGIC) &&
      !buildSidecar(basePath + ".syn", basePath + ".sidx", SIDX_MAGIC, 4, yieldFn, ctx, nullptr)) {
    LOG_ERR("DICT", "Synonym index build failed; synonyms disabled for %s", basePath.c_str());
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
    LOG_ERR("DICT", "OOM: %u byte index scan buffer", static_cast<unsigned>(CHUNK_BYTES));
    return fail(IndexResult::LowMemory);
  }

  // Stream each sample offset straight to the sidecar instead of accumulating
  // them in RAM: a large source would otherwise cost tens of KB of vector heap,
  // and vector growth aborts on OOM under -fno-exceptions. The header slot is
  // zero-filled until the scan succeeds, so an interrupted build leaves a file
  // readSidecarHeader rejects (magic mismatch) and needsIndex() triggers a rebuild.
  HalFile out;
  if (!Storage.openFileForWrite("DICT", sidecarPath, out)) return fail(IndexResult::ReadError);
  const auto writeU32 = [&out](uint32_t v) { return out.write(&v, sizeof(v)) == sizeof(v); };
  const uint32_t placeholder[6] = {};
  bool ok = out.write(placeholder, sizeof(placeholder)) == sizeof(placeholder);
  uint32_t sampleCount = 0;
  if (ok) {
    ok = writeU32(0);  // entry 0 always starts at byte 0
    sampleCount = 1;
  }

  [[maybe_unused]] const unsigned long startMs = millis();  // only read by the LOG_INF below
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

bool Dictionary::openSession(LookupSession& session) {
  if (!isOpen() || !ensureScanBuffers()) return false;

  // One buffer reused for both paths — no transient heap on the lookup path.
  char path[PATH_BUF_BYTES];
  if (!buildPath(path, sizeof(path), ".idx")) return false;
  if (!Storage.openFileForRead("DICT", path, session.idx)) return false;
  session.idxSize = static_cast<uint32_t>(session.idx.fileSize());
  session.idxScan.attach(&session.idx, scanBuf->idx, session.idxSize);

  // The sidecar is optional and its header only has to be validated once per
  // lookup; without a usable one locate() scans from byte 0.
  if (!buildPath(path, sizeof(path), ".qidx")) return true;
  if (Storage.openFileForRead("DICT", path, session.qidx)) {
    const SidecarHeader header = readSidecarHeader(session.qidx, QIDX_MAGIC, SAMPLE_INTERVAL);
    if (header.valid && header.sourceFileSize == session.idxSize) {
      session.sampleCount = header.sampleCount;
      session.entryCount = header.entryCount;
    }
  }
  return true;
}

bool Dictionary::openSynonyms(LookupSession& session) {
  if (session.synOpened) return session.synSize > 0;
  session.synOpened = true;
  if (!hasSyn) return false;

  char path[PATH_BUF_BYTES];
  if (!buildPath(path, sizeof(path), ".syn") || !Storage.openFileForRead("DICT", path, session.syn)) {
    // A .syn exists but won't open: the synonym probe never reached a verdict, so
    // flag it the way openSession() flags an unopenable .idx rather than let
    // lookup() report a genuine miss for a word the .syn does carry.
    session.synFailed = true;
    return false;
  }
  session.synSize = static_cast<uint32_t>(session.syn.fileSize());
  session.synScan.attach(&session.syn, scanBuf->syn, session.synSize);

  if (buildPath(path, sizeof(path), ".sidx") && Storage.openFileForRead("DICT", path, session.sidx)) {
    const SidecarHeader header = readSidecarHeader(session.sidx, SIDX_MAGIC, SAMPLE_INTERVAL);
    if (header.valid && header.sourceFileSize == session.synSize) session.synSampleCount = header.sampleCount;
  }

  // Unlike .qidx, whose absence only costs locate() a bounded-cost scan of an
  // already-open file, an unusable .sidx would make every miss read the whole
  // .syn under the storage mutex. Decline the synonym path instead and release
  // both handles; needsIndex() still reports .sidx stale, so the next index pass
  // rebuilds it.
  if (session.synSampleCount == 0) {
    LOG_ERR("DICT", "Synonym index unusable for %s; synonyms skipped", basePath.c_str());
    session.synFailed = true;
    session.synSize = 0;
    session.sidx.close();
    session.syn.close();
    return false;
  }
  return true;
}

// Shared by locate() (.qidx over .idx) and locateSynonym() (.sidx over .syn):
// both sidecars have the same layout and both sources are sorted word-first, so
// the descent is identical and only the file pair differs.
uint32_t Dictionary::bisectSamples(HalFile& sidecar, ScanReader& source, uint32_t sampleCount, const char* target,
                                   SampleWindow* window) {
  uint32_t startByte = 0;
  if (sampleCount == 0) return startByte;  // no usable sidecar: scan from the start

  // The window a previous descent resolved still answers this target when the
  // target sorts inside it — which is the common case for the stem variants that
  // follow the exact-match probe.
  if (window && window->valid && StringUtils::asciiCaseCmp(window->low.c_str(), target) <= 0 &&
      (!window->hasHigh || StringUtils::asciiCaseCmp(window->high.c_str(), target) > 0)) {
    return window->startByte;
  }
  if (window) window->valid = false;

  uint32_t lo = 0;
  uint32_t hi = sampleCount - 1;
  bool descentOk = true;
  while (lo < hi) {
    const uint32_t mid = (lo + hi + 1) / 2;
    uint32_t offset = 0;
    if (!readSampleOffset(sidecar, mid, &offset) || !source.seekSet(offset) ||
        source.readWord(wordBuf, sizeof(wordBuf)) < 0) {
      lo = 0;  // unreadable sample: abandon the descent and scan from the start
      descentOk = false;
      break;
    }
    if (StringUtils::asciiCaseCmp(wordBuf, target) <= 0) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  if (!readSampleOffset(sidecar, lo, &startByte)) return 0;

  // Record the window so the next probe of this lookup can skip the descent.
  // Its bounds are the headword at the window start and the headword at the next
  // sample — exactly the interval the descent just proved target falls in.
  if (window && descentOk) {
    if (!source.seekSet(startByte) || source.readWord(wordBuf, sizeof(wordBuf)) < 0) return startByte;
    window->low = wordBuf;
    window->hasHigh = false;
    uint32_t nextOffset = 0;
    if (lo + 1 < sampleCount && readSampleOffset(sidecar, lo + 1, &nextOffset) && source.seekSet(nextOffset) &&
        source.readWord(wordBuf, sizeof(wordBuf)) >= 0) {
      window->high = wordBuf;
      window->hasHigh = true;
    }
    window->startByte = startByte;
    window->valid = true;
  }
  return startByte;
}

// Both files stay open across the stem-variant probes; the buffered readers own
// their handles' positions, so a shared handle carries no stale state.
DictLocation Dictionary::locate(LookupSession& session, const char* target, std::string* matchedHeadwordOut) {
  DictLocation result;

  // Bisect the sampled offsets to the last sample whose headword <= target.
  const uint32_t startByte =
      bisectSamples(session.qidx, session.idxScan, session.sampleCount, target, &session.idxWindow);

  // Linear scan of at most SAMPLE_INTERVAL entries: headword NUL, BE32 offset,
  // BE32 size. The index is sorted, so stop at the first headword > target.
  // The exact-case entry of a run is always reached: cleanWord lowercases the
  // target's ASCII letters, so it sorts last among its case variants, and even
  // a descent that lands inside the run starts at or before it.
  if (!session.idxScan.seekSet(startByte)) {
    LOG_ERR("DICT", "Index seek to %lu failed", static_cast<unsigned long>(startByte));
    result.readError = true;
    return result;
  }
  while (session.idxScan.position() < session.idxSize) {
    // Not flagged as readError: readWord returns -1 for EOF and IO error alike,
    // so a short tail can't be told from a truncated .idx. Treat it as the end
    // of the index rather than risk reporting a read failure for what is really
    // a miss.
    if (session.idxScan.readWord(wordBuf, sizeof(wordBuf)) < 0) break;
    uint8_t suffix[8];
    if (!session.idxScan.readBytes(suffix, 8)) break;

    const int cmp = StringUtils::asciiCaseCmp(wordBuf, target);
    if (cmp > 0) break;
    if (cmp < 0) continue;
    bool exact = false;
    if (takesRunEntry(wordBuf, target, result.found, exact)) {
      result.offset = readBe32(suffix);
      result.size = readBe32(suffix + 4);
      result.found = true;
      if (matchedHeadwordOut) *matchedHeadwordOut = wordBuf;
    }
    if (exact) break;
  }
  return result;
}

DictLocation Dictionary::locateByOrdinal(LookupSession& session, uint32_t ordinal, std::string* matchedHeadwordOut) {
  DictLocation result;

  // The .qidx samples are taken at fixed entry-count boundaries (every
  // SAMPLE_INTERVAL entries), so sample (ordinal / SAMPLE_INTERVAL) lands on
  // entry (ordinal / SAMPLE_INTERVAL) * SAMPLE_INTERVAL; scan forward the
  // remainder. Without a usable sidecar, count entries from byte 0.
  uint32_t startByte = 0;
  uint32_t startOrdinal = 0;
  if (session.sampleCount > 0) {
    if (session.entryCount != 0 && ordinal >= session.entryCount) return result;  // out of range
    uint32_t sampleIndex = ordinal / SAMPLE_INTERVAL;
    if (sampleIndex >= session.sampleCount) sampleIndex = session.sampleCount - 1;
    if (readSampleOffset(session.qidx, sampleIndex, &startByte)) {
      startOrdinal = sampleIndex * SAMPLE_INTERVAL;
    } else {
      startByte = 0;
    }
  }

  // Each entry is headword NUL + BE32 offset + BE32 size. Skip to the target,
  // guarding against EOF for a malformed .syn ordinal past the last entry.
  if (!session.idxScan.seekSet(startByte)) {
    LOG_ERR("DICT", "Index seek to %lu failed", static_cast<unsigned long>(startByte));
    result.readError = true;
    return result;
  }
  uint8_t suffix[8];
  for (uint32_t e = startOrdinal; e < ordinal; e++) {
    if (session.idxScan.readWord(wordBuf, sizeof(wordBuf)) < 0 || !session.idxScan.readBytes(suffix, 8)) return result;
    if (session.idxScan.position() >= session.idxSize) return result;  // ordinal past last entry
  }
  if (session.idxScan.position() >= session.idxSize) return result;
  if (session.idxScan.readWord(wordBuf, sizeof(wordBuf)) < 0 || !session.idxScan.readBytes(suffix, 8)) return result;
  result.offset = readBe32(suffix);
  result.size = readBe32(suffix + 4);
  result.found = true;
  if (matchedHeadwordOut) *matchedHeadwordOut = wordBuf;
  return result;
}

DictLocation Dictionary::locateSynonym(LookupSession& session, const char* target, std::string* matchedHeadwordOut) {
  DictLocation result;
  if (!openSynonyms(session)) {
    result.readError = session.synFailed;  // false when there is simply no .syn
    return result;
  }

  // Bisect the sampled offsets to the last synonym <= target, same descent
  // locate() runs over .qidx/.idx. No window cache here: a lookup consults the
  // .syn once, so there is no second probe to save.
  const uint32_t startByte = bisectSamples(session.sidx, session.synScan, session.synSampleCount, target, nullptr);

  // Linear scan of at most SAMPLE_INTERVAL entries: synonym NUL, BE32 ordinal.
  // Sorted, so stop at the first synonym > target, preferring the exact-case
  // entry of a run as locate() does. The ordinal is resolved only once the scan
  // is done, because locateByOrdinal() reuses wordBuf.
  if (!session.synScan.seekSet(startByte)) {
    LOG_ERR("DICT", "Synonym seek to %lu failed", static_cast<unsigned long>(startByte));
    result.readError = true;
    return result;
  }
  bool havePick = false;
  uint32_t pickedOrdinal = 0;
  while (session.synScan.position() < session.synSize) {
    if (session.synScan.readWord(wordBuf, sizeof(wordBuf)) < 0) break;
    uint8_t ordBytes[4];
    if (!session.synScan.readBytes(ordBytes, 4)) break;

    const int cmp = StringUtils::asciiCaseCmp(wordBuf, target);
    if (cmp > 0) break;
    if (cmp < 0) continue;
    bool exact = false;
    if (takesRunEntry(wordBuf, target, havePick, exact)) {
      pickedOrdinal = readBe32(ordBytes);
      havePick = true;
    }
    if (exact) break;
  }
  if (havePick) return locateByOrdinal(session, pickedOrdinal, matchedHeadwordOut);
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

  char pathBuf[PATH_BUF_BYTES];
  if (!buildPath(pathBuf, sizeof(pathBuf), hasPlainDict ? ".dict" : ".dict.dz")) {
    return fail(LookupResult::ReadError);
  }

  // Take the inflate window BEFORE the definition buffer: it is the larger of
  // the two and both come out of the same free run, so asking for it second is
  // what leaves it a few bytes short on a fragmented heap. After the first
  // lookup of a session this is already held and costs nothing.
  if (!hasPlainDict && !dzScratch.ensure()) {
    LOG_ERR("DICT", "Low heap for the dictzip inflate window");
    return fail(LookupResult::LowMemory);
  }

  // Refuse before touching the heap or the SD: std::string growth aborts on OOM
  // (-fno-exceptions), so a definition that would only just fit is refused
  // rather than left to abort mid-assign.
  if (ESP.getMaxAllocHeap() < size + DEFINITION_HEAP_HEADROOM_BYTES) {
    LOG_ERR("DICT", "Low heap for %lu byte definition", static_cast<unsigned long>(size));
    return fail(LookupResult::LowMemory);
  }
  out.assign(size, '\0');

  if (!hasPlainDict) {
    // Inflate straight into the definition buffer. Upstream staged this through
    // a temp file on the SD and read it back, paying a write, a FAT directory
    // update, a reopen and a reread per lookup for bytes it was going to
    // allocate room for anyway.
    DictZip::ExtractError xerr = DictZip::ExtractError::None;
    if (!DictZip::extractEntry(pathBuf, location.offset, size, reinterpret_cast<uint8_t*>(&out[0]), dzScratch, &xerr)) {
      // Map the specific extraction cause to the lookup result: allocation
      // failure (heap fragmentation) vs corrupt/truncated .dz vs an IO error.
      LOG_ERR("DICT", "dictzip extraction failed for %s (error %d)", basePath.c_str(), static_cast<int>(xerr));
      out.clear();
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
    return true;
  }

  HalFile dict;
  if (!Storage.openFileForRead("DICT", pathBuf, dict)) {
    out.clear();
    return fail(LookupResult::ReadError);
  }
  const uint32_t dictSize = static_cast<uint32_t>(dict.fileSize());
  if (location.offset > dictSize || size > dictSize - location.offset) {
    LOG_ERR("DICT", "Definition out of bounds (%lu+%lu > %lu)", static_cast<unsigned long>(location.offset),
            static_cast<unsigned long>(size), static_cast<unsigned long>(dictSize));
    out.clear();
    return fail(LookupResult::ReadError);
  }
  if (!dict.seekSet(location.offset)) {
    LOG_ERR("DICT", "Seek to %lu failed", static_cast<unsigned long>(location.offset));
    out.clear();
    return fail(LookupResult::ReadError);
  }
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
  const auto* b = reinterpret_cast<const unsigned char*>(word);
  size_t start = 0;
  size_t end = strlen(word);
  // Curly quotes, dashes, guillemets and the like are all >= 0x80, so
  // isWordByte keeps them; strip those codepoints from the edges too, or EPUB
  // text like garage.” never matches a headword. See EDGE_PUNCTUATION.
  while (start < end) {
    if (!isWordByte(b[start])) {
      start++;
      continue;
    }
    const size_t len = edgePunctuationLen(b, start, end);
    if (len == 0) break;
    start += len;
  }
  while (end > start) {
    if (!isWordByte(b[end - 1])) {
      end--;
      continue;
    }
    // Back up to the lead byte of the last codepoint (at most 3 continuations).
    size_t lead = end - 1;
    while (lead > start && end - lead < 4 && (b[lead] & 0xC0) == 0x80) lead--;
    const size_t len = edgePunctuationLen(b, lead, end);
    if (len == 0 || lead + len != end) break;
    end = lead;
  }
  if (start >= end) return "";

  std::string result(word + start, end - start);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return c >= 0x80 ? c : static_cast<unsigned char>(std::tolower(c)); });
  return result;
}

// Components of a hyphenated compound, longest first, or empty when the word is
// not hyphenated.
//
// Longest-first rather than left-to-right or right-to-left because the longest
// part is nearly always the one carrying the meaning: "multi-billionaire" ->
// billionaire, "self-esteem" -> esteem, "e-mail" -> mail. It also handles the
// case a head-final rule gets wrong -- "mother-in-law" resolves to "mother"
// rather than "law".
//
// Splits on ASCII hyphen-minus and on U+2010/U+2011, the real hyphens EPUB text
// uses; NOT on en or em dashes, which join clauses rather than build words, and
// which cleanWord has already stripped from the edges.
void Dictionary::hyphenParts(const std::string& word, std::vector<std::string>& out) {
  out.clear();
  const auto* bytes = reinterpret_cast<const unsigned char*>(word.data());
  const size_t n = word.size();

  size_t start = 0;
  for (size_t i = 0; i <= n;) {
    size_t sepLen = 0;
    if (i == n) {
      sepLen = 0;
    } else if (bytes[i] == '-') {
      sepLen = 1;
    } else if (i + 2 < n && bytes[i] == 0xE2 && bytes[i + 1] == 0x80 &&
               (bytes[i + 2] == 0x90 || bytes[i + 2] == 0x91)) {
      sepLen = 3;
    }
    if (sepLen == 0 && i < n) {
      i++;
      continue;
    }
    if (i > start) out.emplace_back(word, start, i - start);
    if (i == n) break;
    i += sepLen;
    start = i;
  }

  // One component means there was no hyphen (or only leading/trailing ones), so
  // there is nothing here the caller has not already tried.
  if (out.size() < 2) {
    out.clear();
    return;
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
}

// The word after a French elided article, pronoun or conjunction -- c' d' j'
// l' m' n' s' t' qu' -- with any of the apostrophes books set (' ’ ʼ), or
// empty when the word does not start with one. cleanWord() has already
// lowercased the ASCII prefix, so "L’Histoire" arrives as "l’Histoire".
//
// Adapted from CrossInk (uxjulia/CrossInk) commit 1babd9da (Julia Nguyen),
// MIT. There the remainder replaces the English stem variants; here it gets
// the full probe, so a French .syn and the plural rule still apply to it.
std::string Dictionary::elisionRemainder(const std::string& word) {
  size_t prefix = 0;
  if (word.compare(0, 2, "qu") == 0) {
    prefix = 2;
  } else if (!word.empty()) {
    switch (word[0]) {
      case 'c':
      case 'd':
      case 'j':
      case 'l':
      case 'm':
      case 'n':
      case 's':
      case 't':
        prefix = 1;
        break;
      default:
        return {};
    }
  }
  if (prefix == 0) return {};
  static constexpr const char* APOSTROPHES[] = {"'", "\xE2\x80\x99", "\xCA\xBC"};  // ' ’ ʼ
  for (const char* apostrophe : APOSTROPHES) {
    const size_t len = strlen(apostrophe);
    if (word.size() > prefix + len && word.compare(prefix, len, apostrophe) == 0) return word.substr(prefix + len);
  }
  return {};
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

bool Dictionary::resolve(const char* word, DictLocation& locationOut, std::string& matchedHeadwordOut,
                         LookupResult* outResult) {
  const auto setResult = [outResult](LookupResult r) {
    if (outResult) *outResult = r;
  };
  setResult(LookupResult::NotFound);
  locationOut = {};
  const std::string cleaned = cleanWord(word);
  if (cleaned.empty() || !isOpen()) return false;

  // One set of open handles for the exact-match probe, the synonym probe and
  // every stem variant, scoped so .idx/.qidx (and .syn/.sidx) close before
  // readDefinition() opens the data file.
  DictLocation location;
  bool searchFailed = false;
  {
    LookupSession session;
    // Couldn't open .idx (or take the scan buffers): the search never reached a
    // verdict, so this is a failure, not a miss.
    if (!openSession(session)) {
      setResult(scanBuf ? LookupResult::ReadError : LookupResult::LowMemory);
      return false;
    }

    // The whole probe for one target: exact, then dictionary-authored synonyms
    // (alternate spellings and irregular forms, which take precedence over the
    // English-only stemmer and are language-agnostic), then stem variants.
    const auto probe = [&](const std::string& target) {
      DictLocation found = locate(session, target.c_str(), &matchedHeadwordOut);
      searchFailed = searchFailed || found.readError;
      if (!found.found && hasSyn) {
        found = locateSynonym(session, target.c_str(), &matchedHeadwordOut);
        searchFailed = searchFailed || found.readError;
      }
      if (!found.found) {
        std::vector<std::string> variants;
        stemVariants(target, variants);
        for (const auto& variant : variants) {
          found = locate(session, variant.c_str(), &matchedHeadwordOut);
          searchFailed = searchFailed || found.readError;
          if (found.found) break;
        }
      }
      return found;
    };

    // cleanWord() lowercases ASCII only, so a sentence-initial or all-caps
    // non-ASCII word ("Über", "STRAẞE", "Привет", "Γεια") misses its lowercase
    // headword. The fully lowercased word is the fallback, never the first try:
    // a dictionary that stores a word capitalised ("Ärzte", "Москва") has to
    // keep its exact hit, and the index cannot fold non-ASCII case in its
    // comparisons, because it is sorted with ASCII-only folding.
    //
    // Based on crosspoint-reader PR #3123 (Esben Sorig, @sorig) and PR #3611
    // (Anton Lijcklama à Nijeholt, @alycklama), reworked onto the shared
    // utf8LowerCodepoint() instead of a German-only fold or a Unicode table.
    const auto probeCased = [&](const std::string& target) {
      DictLocation found = probe(target);
      if (!found.found) {
        const std::string lowered = utf8ToLower(target);
        if (lowered != target) found = probe(lowered);
      }
      return found;
    };

    location = probeCased(cleaned);

    // A French elision ("l'histoire", "qu’après") is no headword, the word
    // after its apostrophe is. Tried after the whole word, which some
    // dictionaries carry ("d'accord"). From here on that bare word stands in
    // for the whole one: "d'avant-garde" splits into "avant" and "garde", not
    // into a "d'avant" no probe would find.
    std::string bare;
    if (!location.found) {
      bare = elisionRemainder(cleaned);
      if (!bare.empty()) location = probeCased(bare);
    }

    // Still nothing, and the word is a hyphenated compound: look up its parts.
    // The compound itself is tried first and often wins -- "mother-in-law" and
    // "well-being" are headwords -- but "multi-billionaire" usually is not, and
    // returning the definition of "billionaire" beats returning nothing.
    if (!location.found) {
      std::vector<std::string> parts;
      hyphenParts(bare.empty() ? cleaned : bare, parts);
      for (const auto& part : parts) {
        location = probeCased(part);
        if (location.found) break;
      }
    }
  }
  if (!location.found) {
    // A search that never reached a verdict (couldn't open or seek .idx) is a
    // read failure, not a miss. Otherwise the word is genuinely not in the
    // dictionary.
    if (searchFailed) setResult(LookupResult::ReadError);
    return false;
  }
  locationOut = location;
  setResult(LookupResult::Found);  // found in the INDEX; reading it may still fail
  return true;
}

bool Dictionary::readResolved(const DictLocation& location, std::string& definitionOut, LookupResult* outResult) {
  // Propagates the precise failure reason from readDefinition (decompression /
  // low memory / read error) so the caller can name it.
  if (readDefinition(location, definitionOut, outResult)) {
    if (outResult) *outResult = LookupResult::Found;
    return true;
  }
  return false;
}

bool Dictionary::lookup(const char* word, std::string& definitionOut, std::string& matchedHeadwordOut,
                        LookupResult* outResult) {
  DictLocation location;
  if (!resolve(word, location, matchedHeadwordOut, outResult)) return false;
  return readResolved(location, definitionOut, outResult);
}
