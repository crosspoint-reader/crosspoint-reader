#include "Dictionary.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "DictZip.h"
#include "DictionaryRegistry.h"
#include "StringUtils.h"

namespace {

// Shared temp file for entries lazily extracted from .dict.dz.
constexpr const char* DICT_TMP_FILE = "/.crosspoint/dict.tmp";

// .qidx sidecar header: magic, version, sample interval, sample count, and the
// .idx file size the sidecar was built from (staleness check).
constexpr uint32_t QIDX_MAGIC = 0x58444951;  // "QIDX" little-endian
constexpr uint32_t QIDX_VERSION = 1;
constexpr size_t QIDX_HEADER_BYTES = 5 * sizeof(uint32_t);

struct QidxHeader {
  uint32_t sampleCount = 0;
  uint32_t idxFileSize = 0;
  bool valid = false;
};

QidxHeader readQidxHeader(HalFile& qidx, uint32_t sampleInterval) {
  QidxHeader header;
  uint32_t raw[5];
  if (!qidx.seekSet(0) || qidx.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) return header;
  if (raw[0] != QIDX_MAGIC || raw[1] != QIDX_VERSION || raw[2] != sampleInterval) return header;
  header.sampleCount = raw[3];
  header.idxFileSize = raw[4];
  header.valid = true;
  return header;
}

bool readSampleOffset(HalFile& qidx, uint32_t sampleIndex, uint32_t* out) {
  if (!qidx.seekSet(QIDX_HEADER_BYTES + static_cast<size_t>(sampleIndex) * sizeof(uint32_t))) return false;
  return qidx.read(out, sizeof(*out)) == static_cast<int>(sizeof(*out));
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

  basePath = std::move(resolved);
  return true;
}

bool Dictionary::needsIndex() {
  if (!isOpen()) return false;

  HalFile idx;
  if (!Storage.openFileForRead("DICT", basePath + ".idx", idx)) return false;
  const uint32_t idxSize = static_cast<uint32_t>(idx.fileSize());

  HalFile qidx;
  if (!Storage.openFileForRead("DICT", basePath + ".qidx", qidx)) return true;
  const QidxHeader header = readQidxHeader(qidx, SAMPLE_INTERVAL);
  return !header.valid || header.idxFileSize != idxSize;
}

bool Dictionary::buildIndex(void (*yieldFn)(void*), void* ctx) {
  if (!isOpen()) return false;

  HalFile idx;
  if (!Storage.openFileForRead("DICT", basePath + ".idx", idx)) return false;
  const uint32_t idxSize = static_cast<uint32_t>(idx.fileSize());

  constexpr size_t CHUNK_BYTES = 4096;
  auto buf = makeUniqueNoThrow<uint8_t[]>(CHUNK_BYTES);
  if (!buf) {
    LOG_ERR("DICT", "OOM: %u byte index scan buffer", CHUNK_BYTES);
    return false;
  }

  // Stream each sample offset straight to the sidecar instead of accumulating
  // them in RAM: a large .idx would otherwise cost tens of KB of vector heap,
  // and vector growth aborts on OOM under -fno-exceptions. The header slot is
  // zero-filled until the scan succeeds, so an interrupted build leaves a file
  // readQidxHeader rejects (magic mismatch) and needsIndex() triggers a rebuild.
  const std::string qidxPath = basePath + ".qidx";
  HalFile out;
  if (!Storage.openFileForWrite("DICT", qidxPath, out)) return false;
  const auto writeU32 = [&out](uint32_t v) { return out.write(&v, sizeof(v)) == static_cast<int>(sizeof(v)); };
  const uint32_t placeholder[5] = {};
  bool ok = out.write(placeholder, sizeof(placeholder)) == sizeof(placeholder);
  uint32_t sampleCount = 0;
  if (ok) {
    ok = writeU32(0);  // entry 0 always starts at byte 0
    sampleCount = 1;
  }

  const unsigned long startMs = millis();
  uint32_t entryCount = 0;
  uint32_t pos = 0;
  uint32_t suffixLeft = 0;  // 0 while scanning a headword, else suffix bytes remaining
  uint32_t sinceYield = 0;
  while (ok && pos < idxSize) {
    const int n = idx.read(buf.get(), CHUNK_BYTES);
    if (n <= 0) {
      LOG_ERR("DICT", "Index scan read failed at %lu", static_cast<unsigned long>(pos));
      ok = false;
      break;
    }
    for (int i = 0; ok && i < n; i++) {
      if (suffixLeft == 0) {
        if (buf[i] == 0) suffixLeft = 8;
      } else if (--suffixLeft == 0) {
        entryCount++;
        const uint32_t nextEntryStart = pos + i + 1;
        if (entryCount % SAMPLE_INTERVAL == 0 && nextEntryStart < idxSize) {
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
    const uint32_t header[5] = {QIDX_MAGIC, QIDX_VERSION, SAMPLE_INTERVAL, sampleCount, idxSize};
    ok = out.seekSet(0) && out.write(header, sizeof(header)) == sizeof(header);
  }
  if (!ok) {
    LOG_ERR("DICT", "Index build failed, removing %s", qidxPath.c_str());
    out.close();  // close before remove of the same path
    Storage.remove(qidxPath.c_str());
    return false;
  }

  LOG_INF("DICT", "Indexed %lu entries (%lu samples) in %lu ms", static_cast<unsigned long>(entryCount),
          static_cast<unsigned long>(sampleCount), millis() - startMs);
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

DictLocation Dictionary::locate(const char* target, std::string* matchedHeadwordOut) {
  DictLocation result;
  if (!isOpen()) return result;

  HalFile idx;
  if (!Storage.openFileForRead("DICT", basePath + ".idx", idx)) return result;
  const uint32_t idxSize = static_cast<uint32_t>(idx.fileSize());

  // Bisect the sampled offsets to the last sample whose headword <= target.
  // Falls back to a full scan from byte 0 when the sidecar is unusable.
  uint32_t startByte = 0;
  HalFile qidx;
  if (Storage.openFileForRead("DICT", basePath + ".qidx", qidx)) {
    const QidxHeader header = readQidxHeader(qidx, SAMPLE_INTERVAL);
    if (header.valid && header.idxFileSize == idxSize && header.sampleCount > 0) {
      uint32_t lo = 0;
      uint32_t hi = header.sampleCount - 1;
      while (lo < hi) {
        const uint32_t mid = (lo + hi + 1) / 2;
        uint32_t offset = 0;
        if (!readSampleOffset(qidx, mid, &offset) || !idx.seekSet(offset) ||
            readWordInto(idx, wordBuf, sizeof(wordBuf)) < 0) {
          lo = 0;
          break;
        }
        if (StringUtils::asciiCaseCmp(wordBuf, target) <= 0) {
          lo = mid;
        } else {
          hi = mid - 1;
        }
      }
      readSampleOffset(qidx, lo, &startByte);
    }
  }

  // Linear scan of at most SAMPLE_INTERVAL entries: headword NUL, BE32 offset,
  // BE32 size. The index is sorted, so stop at the first headword > target.
  idx.seekSet(startByte);
  while (static_cast<uint32_t>(idx.position()) < idxSize) {
    if (readWordInto(idx, wordBuf, sizeof(wordBuf)) < 0) break;
    uint8_t suffix[8];
    if (idx.read(suffix, 8) != 8) break;

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

bool Dictionary::readDefinition(const DictLocation& location, std::string& out) {
  if (!location.found) return false;
  const uint32_t size = std::min(location.size, MAX_DEFINITION_BYTES);

  std::string path;
  uint32_t offset = 0;
  if (hasPlainDict) {
    path = basePath + ".dict";
    offset = location.offset;
  } else {
    HalFile tmp = Storage.open(DICT_TMP_FILE, O_WRITE | O_CREAT | O_TRUNC);
    if (!tmp) {
      LOG_ERR("DICT", "Failed to open %s", DICT_TMP_FILE);
      return false;
    }
    if (!DictZip::extractEntry((basePath + ".dict.dz").c_str(), location.offset, size, tmp)) {
      LOG_ERR("DICT", "dictzip extraction failed for %s", basePath.c_str());
      return false;
    }
    tmp.close();  // close before reopening the same path for read
    path = DICT_TMP_FILE;
  }

  HalFile dict;
  if (!Storage.openFileForRead("DICT", path, dict)) return false;
  const uint32_t dictSize = static_cast<uint32_t>(dict.fileSize());
  if (offset > dictSize || size > dictSize - offset) {
    LOG_ERR("DICT", "Definition out of bounds (%lu+%lu > %lu)", static_cast<unsigned long>(offset),
            static_cast<unsigned long>(size), static_cast<unsigned long>(dictSize));
    return false;
  }

  // std::string growth aborts on OOM (-fno-exceptions); refuse up front unless
  // the allocation fits comfortably in the largest free block.
  if (ESP.getMaxAllocHeap() < size + 8 * 1024) {
    LOG_ERR("DICT", "Low heap for %lu byte definition", static_cast<unsigned long>(size));
    return false;
  }

  dict.seekSet(offset);
  out.assign(size, '\0');
  const int bytesRead = dict.read(&out[0], size);
  if (bytesRead < 0) {
    out.clear();
    return false;
  }
  if (static_cast<uint32_t>(bytesRead) < size) out.resize(bytesRead);
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

bool Dictionary::lookup(const char* word, std::string& definitionOut, std::string& matchedHeadwordOut) {
  const std::string cleaned = cleanWord(word);
  if (cleaned.empty() || !isOpen()) return false;

  DictLocation location = locate(cleaned.c_str(), &matchedHeadwordOut);
  if (!location.found) {
    std::vector<std::string> variants;
    stemVariants(cleaned, variants);
    for (const auto& variant : variants) {
      location = locate(variant.c_str(), &matchedHeadwordOut);
      if (location.found) break;
    }
  }
  if (!location.found) return false;
  return readDefinition(location, definitionOut);
}
