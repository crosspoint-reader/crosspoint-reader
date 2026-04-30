#include "BookSyntheticIndex.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>
#include <expat.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <vector>

#include "htmlEntities.h"

namespace BookSyntheticIndex {
namespace {

constexpr uint32_t kFileMagic = static_cast<uint32_t>('S') | (static_cast<uint32_t>('Y') << 8) |
                                (static_cast<uint32_t>('T') << 16) | (static_cast<uint32_t>('P') << 24);
constexpr uint32_t kFileVersion = 4;
constexpr size_t kZipChunk = 1024;

static size_t countUtf8Codepoints(const char* s, int len) {
  const auto* p = reinterpret_cast<const unsigned char*>(s);
  const auto* end = p + len;
  size_t n = 0;
  while (p < end) {
    const unsigned char* before = p;
    utf8NextCodepoint(&p);
    if (p <= before) p = before + 1;
    n++;
  }
  return n;
}

static const char* localTagName(const XML_Char* name) {
  const char* n = reinterpret_cast<const char*>(name);
  const char* c = std::strrchr(n, ':');
  return c ? c + 1 : n;
}

static bool isHiddenSubtreeTag(const char* tag) {
  static const char* const kHidden[] = {"script", "style", "svg", "title", "noscript", "template", "iframe", "head"};
  for (auto* h : kHidden) {
    if (std::strcmp(tag, h) == 0) return true;
  }
  return false;
}

struct KoreaderSyntheticState {
  uint32_t cpp = 0;
  int count = 0;
  int pageNum = 0;
  uint32_t totalProcessed = 0;
  std::vector<uint32_t> pageStarts;

  void onSpineStart() {
    count = static_cast<int>(cpp);
    pageNum++;
    pageStarts.push_back(totalProcessed);
  }

  void onText(const char* s, int byteLen) {
    const size_t cp0 = countUtf8Codepoints(s, byteLen);
    int lenRem = static_cast<int>(std::min(cp0, static_cast<size_t>(INT_MAX)));
    while (lenRem > count) {
      totalProcessed += static_cast<uint32_t>(count);
      lenRem -= count;
      count = static_cast<int>(cpp);
      pageNum++;
      pageStarts.push_back(totalProcessed);
    }
    totalProcessed += static_cast<uint32_t>(lenRem);
    count -= lenRem;
  }
};

struct ParseMeter {
  KoreaderSyntheticState* st = nullptr;
  int skipDepth = 0;
};

static void feedText(ParseMeter* m, const char* s, int len) {
  if (m->skipDepth > 0 || len <= 0 || m->st == nullptr) return;
  m->st->onText(s, len);
}

static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
  auto* m = static_cast<ParseMeter*>(userData);
  if (m->skipDepth > 0) {
    m->skipDepth++;
    return;
  }
  if (isHiddenSubtreeTag(localTagName(name))) m->skipDepth = 1;
}

static void XMLCALL endElement(void* userData, const XML_Char*) {
  auto* m = static_cast<ParseMeter*>(userData);
  if (m->skipDepth > 0) m->skipDepth--;
}

static void XMLCALL charData(void* userData, const XML_Char* s, int len) {
  feedText(static_cast<ParseMeter*>(userData), reinterpret_cast<const char*>(s), len);
}

static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len) {
  auto* m = static_cast<ParseMeter*>(userData);
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const auto* entity = reinterpret_cast<const char*>(s);
    const char* utf8Value = lookupHtmlEntity(entity, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      feedText(m, utf8Value, static_cast<int>(strlen(utf8Value)));
      return;
    }
    feedText(m, entity, len);
  }
}

/** Streams EPUB ZIP entry into expat (no intermediate file). */
class SpineXmlStream : public Print {
 public:
  bool begin(KoreaderSyntheticState* st) {
    parser_ = XML_ParserCreate(nullptr);
    if (!parser_) return false;
    meter_.st = st;
    XML_SetUserData(parser_, &meter_);
    XML_SetElementHandler(parser_, startElement, endElement);
    XML_SetCharacterDataHandler(parser_, charData);
    XML_SetDefaultHandlerExpand(parser_, defaultHandlerExpand);
    return true;
  }

  ~SpineXmlStream() override {
    if (parser_) XML_ParserFree(parser_);
  }

  bool finalize() {
    if (!parser_ || fatal_) return false;
    fatal_ = fatal_ || (XML_Parse(parser_, "", 0, 1) == XML_STATUS_ERROR);
    if (fatal_) {
      LOG_ERR("SYTP", "XML finalize: %s", XML_ErrorString(XML_GetErrorCode(parser_)));
      return false;
    }
    XML_ParserFree(parser_);
    parser_ = nullptr;
    return true;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buf, size_t len) override {
    if (!parser_ || fatal_ || len == 0) return len;
    if (XML_Parse(parser_, reinterpret_cast<const char*>(buf), static_cast<int>(len), 0) == XML_STATUS_ERROR) {
      LOG_ERR("SYTP", "XML chunk: %s", XML_ErrorString(XML_GetErrorCode(parser_)));
      fatal_ = true;
    }
    return len;
  }

 private:
  XML_Parser parser_ = nullptr;
  ParseMeter meter_{};
  bool fatal_ = false;
};

static bool parseOneSpine(const Epub& epub, const std::string& itemHref, KoreaderSyntheticState& st) {
  SpineXmlStream sink;
  if (!sink.begin(&st)) return false;
  if (!epub.readItemContentsToStream(itemHref, sink, kZipChunk)) return false;
  return sink.finalize();
}

}  // namespace

bool loadFromCache(const std::string& cachePath, const int spineCount, const uint32_t cpp, BuiltIndex& out) {
  out = {};
  if (spineCount <= 0) return false;

  FsFile f;
  if (!Storage.openFileForRead("SYTP", (cachePath + "/book_synthetic_pages.bin").c_str(), f)) return false;

  auto rd32 = [&](uint32_t& v) { return f.read(reinterpret_cast<uint8_t*>(&v), sizeof(v)) == sizeof(v); };

  uint32_t magic = 0, version = 0, storedSpine = 0, storedCpp = 0;
  uint32_t totalPages = 0, totalText = 0;
  if (!rd32(magic) || magic != kFileMagic) return false;
  if (!rd32(version) || version != kFileVersion) return false;
  if (!rd32(storedSpine) || storedSpine != static_cast<uint32_t>(spineCount)) return false;
  if (!rd32(storedCpp) || storedCpp != cpp) return false;
  if (!rd32(totalPages) || !rd32(totalText)) return false;

  out.pageStartChar.resize(static_cast<size_t>(totalPages));
  for (uint32_t i = 0; i < totalPages; i++) {
    if (!rd32(out.pageStartChar[i])) return false;
  }

  out.spineFirstSyntheticPage.resize(static_cast<size_t>(spineCount));
  for (uint32_t& spineFirst : out.spineFirstSyntheticPage) {
    if (!rd32(spineFirst)) return false;
  }

  out.charsPerPage = cpp;
  out.totalPages = totalPages;
  out.totalTextCodepoints = totalText;

  if (out.totalPages == 0 || out.pageStartChar.empty()) {
    out = {};
    return false;
  }
  return true;
}

bool buildAndSave(const Epub& epub, const uint32_t cpp, BuiltIndex& out, BuildProgressFn onProgress) {
  out = {};
  if (cpp == 0) return false;
  const int spineCount = epub.getSpineItemsCount();
  if (spineCount <= 0) return false;

  if (onProgress) onProgress(0, spineCount);

  KoreaderSyntheticState st{};
  st.cpp = cpp;
  out.spineFirstSyntheticPage.clear();
  out.spineFirstSyntheticPage.reserve(static_cast<size_t>(spineCount));
  for (int i = 0; i < spineCount; i++) {
    st.onSpineStart();
    out.spineFirstSyntheticPage.push_back(static_cast<uint32_t>(st.pageNum));
    if (!parseOneSpine(epub, epub.getSpineItem(i).href, st)) {
      LOG_DBG("SYTP", "Parse spine %d failed (%s)", i, epub.getSpineItem(i).href.c_str());
    }
    if (onProgress) onProgress(i + 1, spineCount);
  }

  if (st.pageNum < 1 || st.pageStarts.size() != static_cast<size_t>(st.pageNum)) {
    LOG_ERR("SYTP", "Inconsistent synthetic page state");
    out = {};
    return false;
  }

  out.charsPerPage = cpp;
  out.totalPages = static_cast<uint32_t>(st.pageNum);
  out.totalTextCodepoints = st.totalProcessed;
  out.pageStartChar = std::move(st.pageStarts);

  FsFile f;
  const std::string path = epub.getCachePath() + "/book_synthetic_pages.bin";
  if (!Storage.openFileForWrite("SYTP", path.c_str(), f)) {
    LOG_ERR("SYTP", "Could not write %s", path.c_str());
    return false;
  }

  uint32_t magic = kFileMagic;
  uint32_t version = kFileVersion;
  uint32_t sc = static_cast<uint32_t>(spineCount);
  f.write(reinterpret_cast<const uint8_t*>(&magic), sizeof(magic));
  f.write(reinterpret_cast<const uint8_t*>(&version), sizeof(version));
  f.write(reinterpret_cast<const uint8_t*>(&sc), sizeof(sc));
  f.write(reinterpret_cast<const uint8_t*>(&cpp), sizeof(cpp));
  f.write(reinterpret_cast<const uint8_t*>(&out.totalPages), sizeof(out.totalPages));
  f.write(reinterpret_cast<const uint8_t*>(&out.totalTextCodepoints), sizeof(out.totalTextCodepoints));
  for (uint32_t v : out.pageStartChar) {
    f.write(reinterpret_cast<const uint8_t*>(&v), sizeof(v));
  }
  for (uint32_t v : out.spineFirstSyntheticPage) {
    f.write(reinterpret_cast<const uint8_t*>(&v), sizeof(v));
  }

  LOG_DBG("SYTP", "Saved synthetic index: %u pages", static_cast<unsigned>(out.totalPages));
  return true;
}

}  // namespace BookSyntheticIndex
