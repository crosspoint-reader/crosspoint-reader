#include "DictLookup.h"

#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr const char* PAGES_PATH = "/shortbread/dict/pages.idx";
constexpr const char* WORDS_PATH = "/shortbread/dict/words.idx";
constexpr const char* DEFS_PATH  = "/shortbread/dict/defs.bin";
constexpr uint8_t  EXPECTED_VERSION = 1;
constexpr uint32_t MAX_PAGE_SIZE   = 8192;
constexpr uint32_t MAX_DEF_SIZE    = 16 * 1024;  // decompressed cap

std::string toLower(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}
}  // namespace

bool DictLookup::load() {
  if (loaded) return true;

  FsFile f;
  if (!Storage.openFileForRead("DICT", PAGES_PATH, f)) {
    LOG_DBG("DICT", "pages.idx not found at %s", PAGES_PATH);
    return false;
  }

  uint8_t magic[4];
  if (f.read(magic, 4) != 4 || std::memcmp(magic, "DICT", 4) != 0) {
    LOG_DBG("DICT", "bad magic");
    return false;
  }
  uint8_t version = 0;
  if (f.read(&version, 1) != 1 || version != EXPECTED_VERSION) {
    LOG_DBG("DICT", "bad version %u", version);
    return false;
  }
  uint16_t pageSz = 0;
  uint32_t pageCnt = 0;
  uint32_t entryCnt = 0;
  if (f.read(reinterpret_cast<uint8_t*>(&pageSz), 2) != 2) return false;
  if (f.read(reinterpret_cast<uint8_t*>(&pageCnt), 4) != 4) return false;
  if (f.read(reinterpret_cast<uint8_t*>(&entryCnt), 4) != 4) return false;
  if (pageSz == 0 || pageSz > MAX_PAGE_SIZE || pageCnt == 0) {
    LOG_DBG("DICT", "bad header pageSize=%u pageCount=%u", pageSz, pageCnt);
    return false;
  }

  pages.clear();
  pages.reserve(pageCnt);
  for (uint32_t i = 0; i < pageCnt; ++i) {
    uint8_t wlen = 0;
    if (f.read(&wlen, 1) != 1) {
      LOG_DBG("DICT", "truncated pages.idx at page %u", i);
      return false;
    }
    std::string w(wlen, '\0');
    if (wlen > 0 && f.read(reinterpret_cast<uint8_t*>(&w[0]), wlen) != wlen) {
      LOG_DBG("DICT", "truncated page word %u", i);
      return false;
    }
    pages.push_back({std::move(w)});
  }

  pageSize = pageSz;
  pageCount = pageCnt;
  entryCount = entryCnt;
  loaded = true;
  LOG_DBG("DICT", "loaded %u entries across %u pages", entryCount, pageCount);
  return true;
}

std::string DictLookup::lookup(const std::string& wordIn) const {
  if (!loaded || pages.empty()) return {};
  const std::string word = toLower(wordIn);
  if (word.empty()) return {};

  // Binary search pages: find largest i where pages[i].firstWord <= word.
  size_t lo = 0;
  size_t hi = pages.size();
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (pages[mid].firstWord <= word) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) return {};
  size_t pageIdx = lo - 1;

  FsFile wf;
  if (!Storage.openFileForRead("DICT", WORDS_PATH, wf)) return {};
  if (!wf.seek(static_cast<size_t>(pageIdx) * pageSize)) return {};

  std::vector<uint8_t> buf(pageSize);
  if (wf.read(buf.data(), pageSize) != static_cast<int>(pageSize)) return {};

  size_t pos = 0;
  uint32_t defOff = 0;
  uint32_t defLen = 0;
  bool found = false;
  while (pos < pageSize) {
    uint8_t wlen = buf[pos];
    if (wlen == 0) break;
    if (pos + 1 + wlen + 8 > pageSize) break;
    const char* wptr = reinterpret_cast<const char*>(&buf[pos + 1]);
    int cmp = std::memcmp(word.data(), wptr,
                          std::min<size_t>(word.size(), wlen));
    if (cmp == 0 && word.size() == wlen) {
      std::memcpy(&defOff, &buf[pos + 1 + wlen], 4);
      std::memcpy(&defLen, &buf[pos + 1 + wlen + 4], 4);
      found = true;
      break;
    }
    if (cmp == 0 && word.size() < static_cast<size_t>(wlen)) break;
    if (cmp < 0) break;  // sorted; word would have appeared already
    pos += 1 + wlen + 8;
  }
  if (!found) return {};
  if (defLen == 0 || defLen > MAX_DEF_SIZE) return {};

  FsFile df;
  if (!Storage.openFileForRead("DICT", DEFS_PATH, df)) return {};
  if (!df.seek(defOff)) return {};

  std::vector<uint8_t> compressed(defLen);
  if (df.read(compressed.data(), defLen) != static_cast<int>(defLen)) return {};

  InflateReader inflater;
  if (!inflater.init(false)) return {};
  inflater.setSource(compressed.data(), compressed.size());

  std::string out;
  out.resize(MAX_DEF_SIZE);
  size_t produced = 0;
  InflateStatus st = inflater.readAtMost(reinterpret_cast<uint8_t*>(&out[0]),
                                         out.size(), &produced);
  if (st == InflateStatus::Error) return {};
  out.resize(produced);
  return out;
}
