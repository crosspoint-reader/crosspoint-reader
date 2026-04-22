#include "BookSyntheticIndex.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>
#include <expat.h>

#include <algorithm>
#include <climits>
#include <cstring>

#include "htmlEntities.h"

namespace BookSyntheticIndex {

namespace {

constexpr uint32_t kFileMagic = static_cast<uint32_t>('S') | (static_cast<uint32_t>('Y') << 8) |
                                (static_cast<uint32_t>('T') << 16) | (static_cast<uint32_t>('P') << 24);
constexpr uint32_t kFileVersion = 3;
constexpr size_t kParseBufferSize = 1024;

static size_t countUtf8Codepoints(const char* s, int len) {
  const auto* p = reinterpret_cast<const unsigned char*>(s);
  const auto* end = p + len;
  size_t n = 0;
  while (p < end) {
    const unsigned char* before = p;
    utf8NextCodepoint(&p);
    if (p <= before) {
      p = before + 1;
    }
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
  return std::strcmp(tag, "script") == 0 || std::strcmp(tag, "style") == 0 || std::strcmp(tag, "svg") == 0 ||
         std::strcmp(tag, "title") == 0 || std::strcmp(tag, "noscript") == 0 || std::strcmp(tag, "template") == 0 ||
         std::strcmp(tag, "iframe") == 0 || std::strcmp(tag, "head") == 0;
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
  if (isHiddenSubtreeTag(localTagName(name))) {
    m->skipDepth = 1;
  }
}

static void XMLCALL endElement(void* userData, const XML_Char*) {
  auto* m = static_cast<ParseMeter*>(userData);
  if (m->skipDepth > 0) {
    m->skipDepth--;
  }
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

static bool parseOneSpine(const Epub& epub, const std::string& itemHref, KoreaderSyntheticState& st) {
  const std::string path = FsHelpers::normalisePath(itemHref);
  const std::string tmpPath = epub.getCachePath() + "/.syntp_parse.tmp";
  Storage.remove(tmpPath.c_str());

  FsFile out;
  if (!Storage.openFileForWrite("SYTP", tmpPath.c_str(), out)) {
    return false;
  }
  if (!epub.readItemContentsToStream(itemHref, out, kParseBufferSize)) {
    out.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  out.close();

  FsFile in;
  if (!Storage.openFileForRead("SYTP", tmpPath.c_str(), in)) {
    Storage.remove(tmpPath.c_str());
    return false;
  }

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    in.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  ParseMeter meter{};
  meter.st = &st;
  XML_SetUserData(parser, &meter);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, charData);
  XML_SetDefaultHandlerExpand(parser, defaultHandlerExpand);

  bool ok = true;
  int done = 0;
  do {
    void* const buf = XML_GetBuffer(parser, static_cast<int>(kParseBufferSize));
    if (!buf) {
      ok = false;
      break;
    }
    const size_t len = in.read(buf, kParseBufferSize);
    if (len == 0 && in.available() > 0) {
      ok = false;
      break;
    }
    done = in.available() == 0 ? 1 : 0;
    if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      LOG_ERR("SYTP", "XML parse error in %s: %s", path.c_str(), XML_ErrorString(XML_GetErrorCode(parser)));
      ok = false;
      break;
    }
  } while (!done);

  XML_ParserFree(parser);
  in.close();
  Storage.remove(tmpPath.c_str());
  return ok;
}

}  // namespace

bool loadFromCache(const std::string& cachePath, const int spineCount, const uint32_t cpp, BuiltIndex& out) {
  out = {};
  if (spineCount <= 0) {
    return false;
  }

  FsFile f;
  if (!Storage.openFileForRead("SYTP", (cachePath + "/book_synthetic_pages.bin").c_str(), f)) {
    return false;
  }

  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t storedSpine = 0;
  uint32_t storedCpp = 0;
  uint32_t totalPages = 0;
  uint32_t totalText = 0;
  if (f.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic)) != sizeof(magic) || magic != kFileMagic) {
    return false;
  }
  if (f.read(reinterpret_cast<uint8_t*>(&version), sizeof(version)) != sizeof(version) || version != kFileVersion) {
    return false;
  }
  if (f.read(reinterpret_cast<uint8_t*>(&storedSpine), sizeof(storedSpine)) != sizeof(storedSpine) ||
      storedSpine != static_cast<uint32_t>(spineCount)) {
    return false;
  }
  if (f.read(reinterpret_cast<uint8_t*>(&storedCpp), sizeof(storedCpp)) != sizeof(storedCpp) || storedCpp != cpp) {
    return false;
  }
  if (f.read(reinterpret_cast<uint8_t*>(&totalPages), sizeof(totalPages)) != sizeof(totalPages) ||
      f.read(reinterpret_cast<uint8_t*>(&totalText), sizeof(totalText)) != sizeof(totalText)) {
    return false;
  }

  out.pageStartChar.resize(static_cast<size_t>(totalPages));
  for (uint32_t i = 0; i < totalPages; i++) {
    uint32_t v = 0;
    if (f.read(reinterpret_cast<uint8_t*>(&v), sizeof(v)) != sizeof(v)) {
      return false;
    }
    out.pageStartChar[i] = v;
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
  if (cpp == 0) {
    return false;
  }
  const int spineCount = epub.getSpineItemsCount();
  if (spineCount <= 0) {
    return false;
  }

  if (onProgress) {
    onProgress(0, spineCount);
  }

  KoreaderSyntheticState st;
  st.cpp = cpp;
  for (int i = 0; i < spineCount; i++) {
    st.onSpineStart();
    if (!parseOneSpine(epub, epub.getSpineItem(i).href, st)) {
      LOG_DBG("SYTP", "Parse failed or empty spine %d (%s)", i, epub.getSpineItem(i).href.c_str());
    }
    if (onProgress) {
      onProgress(i + 1, spineCount);
    }
  }

  if (st.pageNum < 1 || st.pageStarts.size() != static_cast<size_t>(st.pageNum)) {
    LOG_ERR("SYTP", "Inconsistent synthetic page state");
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

  LOG_DBG("SYTP", "Saved synthetic index: %u pages, %u codepoints", static_cast<unsigned>(out.totalPages),
          static_cast<unsigned>(out.totalTextCodepoints));
  return true;
}

}  // namespace BookSyntheticIndex
