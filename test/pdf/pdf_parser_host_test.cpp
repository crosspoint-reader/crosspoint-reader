/**
 * Host-side integration test for lib/Pdf (real sources, in-memory HalFile stub).
 * Build: test/pdf/run_pdf_parser_tests.sh
 */

#include "ContentStream.h"
#include "PageTree.h"
#include "PdfCachedPageReader.h"
#include "PdfObject.h"
#include "PdfOutline.h"
#include "PdfPage.h"
#include "PdfPageNavigation.h"
#include "XrefTable.h"

#include <HalStorage.h>
#include <InflateReader.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <zlib.h>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* expr, const char* file, int line) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s:%d  (%s)\n", file, line, expr);
    ++g_failures;
  }
}

#define REQUIRE(cond) check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

// Stop this file's checks on first failure chain (avoids cascading bogus failures).
#define REQF(cond)                       \
  do {                                   \
    if (!(cond)) {                       \
      REQUIRE(cond);                     \
      return false;                      \
    }                                    \
  } while (0)

uint32_t contentsObjectId(const std::string& pageBody) {
  PdfFixedString<PDF_DICT_VALUE_MAX> cv;
  if (!PdfObject::getDictValue("/Contents", pageBody, cv)) {
    return 0;
  }
  while (!cv.empty() && (cv[0] == ' ' || cv[0] == '\t' || cv[0] == '\r' || cv[0] == '\n')) {
    cv.erase_prefix(1);
  }
  if (cv.empty()) return 0;
  if (!cv.empty() && cv[0] == '[') {
    const char* p = cv.c_str();
    while (*p && *p != '[') ++p;
    if (*p == '[') ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    char* end = nullptr;
    const unsigned long id = std::strtoul(p, &end, 10);
    if (end == p) return 0;
    return static_cast<uint32_t>(id);
  }
  return PdfObject::getDictRef("/Contents", pageBody);
}

bool parseSinglePage(HalFile& file, const XrefTable& xref, const std::string& pageBody, PdfPage& outPage) {
  const uint32_t contentId = contentsObjectId(pageBody);
  if (contentId == 0) return false;
  PdfFixedString<PDF_OBJECT_BODY_MAX> contentDict;
  bool compressed = false;
  uint32_t streamOffset = 0;
  uint32_t streamLen = 0;
  if (!xref.readStreamMetaForObject(file, contentId, contentDict, streamOffset, streamLen, compressed)) {
    PdfByteBuffer payload;
    if (!xref.readStreamForObject(file, contentId, contentDict, payload, compressed)) return false;
    if (payload.len == 0) return false;
    return ContentStream::parseBuffer(payload.ptr(), payload.len, compressed, file, xref, pageBody, outPage);
  }
  return ContentStream::parse(file, streamOffset, streamLen, compressed, xref, pageBody, outPage);
}

struct Expectation {
  const char* filename;
  unsigned minPages;
  unsigned minOutlineEntries;  // 0 = do not check outline count
  unsigned minTextCharsPage0;  // UTF-8 bytes ~chars
  bool requireTextOnPage0;
  unsigned minTextCharsPage1;  // 0 = skip; page 1 is index 1 (second page)
};

// Thresholds are conservative: real PDFs vary; tighten if a file regresses.
const Expectation kExpect[] = {
    {"sample.pdf", 1, 0, 20, true, 0},
    // Cover + body often use CMap/CID fonts; pipeline check only until encoding support improves.
    {"EE-366.pdf", 1, 0, 500, true, 0},
    {"esp32-c6_datasheet_en.pdf", 20, 0, 10, true, 0},
    // Clinical handbook: xref + page tree + outlines + content streams parse; no text yet (CID/CMap).
    {"Problem-Solving Treatment_ Learning and Pl - IHS.pdf", 33, 33, 200, true, 0},
    {"DBT(r) Skills Training Handouts and Worksh - Marsha M Linehan.pdf", 446, 0, 0, false, 0},
};

const Expectation* findExpect(const char* base) {
  for (const auto& e : kExpect) {
    if (std::strcmp(base, e.filename) == 0) return &e;
  }
  return nullptr;
}

size_t totalTextChars(const PdfPage& p) {
  size_t n = 0;
  for (const auto& b : p.textBlocks) n += b.text.size();
  return n;
}

bool hasStyle(uint8_t style, uint8_t flag) {
  return (style & flag) != 0;
}

const char* styleName(uint8_t style) {
  if (hasStyle(style, PdfTextStyleHeader)) {
    return "header";
  }
  if (hasStyle(style, PdfTextStyleBold) && hasStyle(style, PdfTextStyleItalic)) {
    return "bold+italic";
  }
  if (hasStyle(style, PdfTextStyleBold)) {
    return "bold";
  }
  if (hasStyle(style, PdfTextStyleItalic)) {
    return "italic";
  }
  return "regular";
}

void dumpPageText(size_t pageIndex, const PdfPage& page) {
  std::printf("----- page %zu -----\n", pageIndex + 1);
  for (const auto& block : page.textBlocks) {
    const std::string text = std::string(block.text.view());
    if (!text.empty()) {
      std::printf("[%s] %s\n", styleName(block.style), text.c_str());
    }
  }
  std::printf("\n");
}

void testPdfPageNavigationPolicy() {
  PdfPageNavigationState state{};
  REQUIRE(state.page == 0);
  REQUIRE(state.slice == 0);

  REQUIRE(pdfPageTurnForward(state, 10, 3));
  REQUIRE(state.page == 0);
  REQUIRE(state.slice == 1);

  REQUIRE(pdfPageTurnForward(state, 10, 3));
  REQUIRE(state.page == 0);
  REQUIRE(state.slice == 2);

  REQUIRE(pdfPageTurnForward(state, 10, 3));
  REQUIRE(state.page == 1);
  REQUIRE(state.slice == 0);

  REQUIRE(pdfPageTurnBackward(state, 3));
  REQUIRE(state.page == 0);
  REQUIRE(state.slice == 2);
}

void testPdfCachedPageReader() {
  const std::filesystem::path cacheRoot = std::filesystem::path("test/pdf/build") / "pdf-cache-reader-test";
  std::filesystem::create_directories(cacheRoot / "pages");
  const std::filesystem::path pagePath = cacheRoot / "pages" / "0.bin";
  FILE* f = std::fopen(pagePath.string().c_str(), "wb");
  REQUIRE(f != nullptr);
  auto writeU8 = [&](uint8_t v) { REQUIRE(std::fwrite(&v, 1, 1, f) == 1); };
  auto writeU16 = [&](uint16_t v) { REQUIRE(std::fwrite(&v, sizeof(v), 1, f) == 1); };
  auto writeU32 = [&](uint32_t v) { REQUIRE(std::fwrite(&v, sizeof(v), 1, f) == 1); };
  auto writeBytes = [&](const void* p, size_t n) { REQUIRE(std::fwrite(p, 1, n, f) == n); };

  writeU8(5);
  writeU32(1);
  const char text[] = "hello cached page";
  writeU32(static_cast<uint32_t>(sizeof(text) - 1));
  writeBytes(text, sizeof(text) - 1);
  writeU8(static_cast<uint8_t>(PdfTextStyleBold | PdfTextStyleHeader));
  writeU32(42);
  writeU32(1);
  writeU32(111);
  writeU32(222);
  writeU16(33);
  writeU16(44);
  writeU8(1);
  writeU32(2);
  writeU8(0);
  writeU32(0);
  writeU8(1);
  writeU32(0);
  REQUIRE(std::fclose(f) == 0);

  PdfCachedPageReader reader;
  REQUIRE(reader.open(cacheRoot.c_str(), 0));
  REQUIRE(reader.textCount() == 1);
  REQUIRE(reader.imageCount() == 1);
  REQUIRE(reader.drawCount() == 2);

  PdfTextBlock loadedText;
  REQUIRE(reader.loadTextBlock(0, loadedText));
  REQUIRE(loadedText.text.view() == "hello cached page");
  REQUIRE(loadedText.style == static_cast<uint8_t>(PdfTextStyleBold | PdfTextStyleHeader));
  REQUIRE(loadedText.orderHint == 42);

  PdfImageDescriptor loadedImg{};
  REQUIRE(reader.loadImage(0, loadedImg));
  REQUIRE(loadedImg.pdfStreamOffset == 111);
  REQUIRE(loadedImg.pdfStreamLength == 222);
  REQUIRE(loadedImg.width == 33);
  REQUIRE(loadedImg.height == 44);
  REQUIRE(loadedImg.format == 1);

  PdfDrawStep loadedStep{};
  REQUIRE(reader.loadDrawStep(0, loadedStep));
  REQUIRE(!loadedStep.isImage);
  REQUIRE(loadedStep.index == 0);
  REQUIRE(reader.loadDrawStep(1, loadedStep));
  REQUIRE(loadedStep.isImage);
  REQUIRE(loadedStep.index == 0);
}

struct InflateStreamCtx {
  InflateReader reader;  // must be first for callback context cast
  const uint8_t* compressed = nullptr;
  size_t compressedLen = 0;
  size_t readOffset = 0;
  uint8_t readBuf[256]{};
};

static int inflateStreamCallback(uzlib_uncomp* uncomp) {
  auto* ctx = reinterpret_cast<InflateStreamCtx*>(uncomp);
  if (ctx->readOffset >= ctx->compressedLen) return -1;

  const size_t remaining = ctx->compressedLen - ctx->readOffset;
  const size_t toRead = remaining < sizeof(ctx->readBuf) ? remaining : sizeof(ctx->readBuf);
  std::memcpy(ctx->readBuf, ctx->compressed + ctx->readOffset, toRead);
  ctx->readOffset += toRead;
  uncomp->source = ctx->readBuf + 1;
  uncomp->source_limit = ctx->readBuf + toRead;
  return ctx->readBuf[0];
}

void testInflateReaderLongWindowStreaming() {
  const size_t plainLen = 70000;
  std::string plain(plainLen, 'A');
  for (size_t i = 0; i < plainLen; ++i) {
    plain[i] = static_cast<char>('A' + (i % 26));
  }

  uLongf compressedBound = compressBound(plain.size());
  REQUIRE(compressedBound > 0);
  std::vector<uint8_t> compressed(compressedBound);
  const auto zret = compress2(
      reinterpret_cast<Bytef*>(compressed.data()), &compressedBound, reinterpret_cast<const Bytef*>(plain.data()), plain.size(),
      Z_BEST_COMPRESSION);
  REQUIRE(zret == Z_OK);
  compressed.resize(compressedBound);

  InflateStreamCtx ctx;
  ctx.compressed = compressed.data();
  ctx.compressedLen = compressed.size();
  REQUIRE(ctx.reader.init(true));
  ctx.reader.setReadCallback(inflateStreamCallback);

  std::vector<uint8_t> out;
  out.resize(plainLen);
  size_t producedTotal = 0;
  while (producedTotal < plainLen) {
    size_t produced = 0;
    const auto status = ctx.reader.readAtMost(out.data() + producedTotal, plainLen - producedTotal, &produced);
    producedTotal += produced;
    if (status == InflateStatus::Error) {
      REQUIRE(status != InflateStatus::Error);
      return;
    }
    if (status == InflateStatus::Done) {
      break;
    }
  }

  REQUIRE(producedTotal == plainLen);
  REQUIRE(std::memcmp(out.data(), plain.data(), plainLen) == 0);
}

void collapseAsciiWhitespace(std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool pendingSpace = false;
  for (size_t i = 0; i < s.size();) {
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    if (b0 == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) {
      pendingSpace = true;
      i += 2;
      continue;
    }
    if (b0 == ' ' || b0 == '\t' || b0 == '\r' || b0 == '\n') {
      pendingSpace = true;
      i += 1;
      continue;
    }
    if (pendingSpace && !out.empty()) {
      out.push_back(' ');
    }
    pendingSpace = false;
    size_t n = 1;
    if (b0 >= 0x80) {
      if ((b0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
        n = 2;
      } else if ((b0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
        n = 3;
      } else if ((b0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
        n = 4;
      }
    }
    for (size_t j = 0; j < n && i + j < s.size(); ++j) {
      out.push_back(s[i + j]);
    }
    i += n;
  }
  s = std::move(out);
}

void printFirstTextBlockPreview(const char* label, const PdfPage& page) {
  for (const auto& b : page.textBlocks) {
    std::string s = std::string(b.text.view());
    collapseAsciiWhitespace(s);
    if (s.empty()) {
      continue;
    }
    constexpr size_t kMax = 240;
    if (s.size() > kMax) {
      s = s.substr(0, kMax);
      s += "...";
    }
    std::printf("     %s: %s\n", label, s.c_str());
    return;
  }
  std::printf("     %s: (no non-empty blocks)\n", label);
}

bool runOnePdf(const char* path) {
  HalFile file;
  REQF(file.loadPath(path));
  const char* slash = std::strrchr(path, '/');
  const char* base = slash ? slash + 1 : path;

  XrefTable xref;
  REQF(xref.parse(file));

  std::string catalogBody;
  PdfFixedString<PDF_OBJECT_BODY_MAX> catalogBodyFixed;
  const uint32_t rootId = xref.rootObjId();
  REQF(rootId != 0);
  REQF(xref.readDictForObject(file, rootId, catalogBodyFixed));
  catalogBody = std::string(catalogBodyFixed.view());

  const uint32_t pagesObjId = PdfObject::getDictRef("/Pages", catalogBody);
  REQF(pagesObjId != 0);

  PageTree pageTree;
  REQF(pageTree.parse(file, xref, pagesObjId));
  const uint32_t pageCount = pageTree.pageCount();
  REQF(pageCount > 0);

  const Expectation* exp = findExpect(base);
  if (exp) {
    REQF(pageCount >= exp->minPages);
  }

  PdfFixedVector<PdfOutlineEntry, PDF_MAX_OUTLINE_ENTRIES> outline;
  const uint32_t outlinesId = PdfObject::getDictRef("/Outlines", catalogBody);
  if (outlinesId != 0) {
    REQF(PdfOutlineParser::parse(file, xref, pageTree, outlinesId, outline));
    if (exp && exp->minOutlineEntries > 0) {
      REQF(outline.size() >= exp->minOutlineEntries);
    }
  }

  PdfPage page0;
  {
    const uint32_t pageObjId = pageTree.getPageObjectId(0);
    REQF(pageObjId != 0);
    PdfFixedString<PDF_OBJECT_BODY_MAX> pageBodyFixed;
    REQF(xref.readDictForObject(file, pageObjId, pageBodyFixed));
    std::string pageBody(pageBodyFixed.view());
    REQF(parseSinglePage(file, xref, pageBody, page0));
  }
  const bool dumpText = std::getenv("PDF_TEST_DUMP_TEXT") != nullptr;
  if (dumpText) {
    dumpPageText(0, page0);
  }

  if (std::strcmp(base, "Problem-Solving Treatment_ Learning and Pl - IHS.pdf") == 0) {
    REQUIRE(!page0.textBlocks.empty());
    REQUIRE(hasStyle(page0.textBlocks[0].style, PdfTextStyleHeader));
  }
  if (std::strcmp(base, "EE-366.pdf") == 0) {
    bool sawBold = false;
    bool sawItalic = false;
    for (const auto& block : page0.textBlocks) {
      if (hasStyle(block.style, PdfTextStyleBold)) {
        sawBold = true;
      }
      if (hasStyle(block.style, PdfTextStyleItalic)) {
        sawItalic = true;
      }
    }
    REQUIRE(sawBold);
    REQUIRE(sawItalic);
  }

  if (exp && exp->requireTextOnPage0) {
    REQF(totalTextChars(page0) >= exp->minTextCharsPage0);
  }

  if (pageCount > 1) {
    PdfPage page1;
    const uint32_t pageObjId = pageTree.getPageObjectId(1);
    REQF(pageObjId != 0);
    PdfFixedString<PDF_OBJECT_BODY_MAX> pageBodyFixed;
    REQF(xref.readDictForObject(file, pageObjId, pageBodyFixed));
    std::string pageBody(pageBodyFixed.view());
    REQF(parseSinglePage(file, xref, pageBody, page1));
    if (dumpText) {
      dumpPageText(1, page1);
    }
    if (exp && exp->minTextCharsPage1 > 0) {
      REQF(totalTextChars(page1) >= exp->minTextCharsPage1);
    }
  } else if (exp && exp->minTextCharsPage1 > 0) {
    REQF(false);
  }
  if (dumpText && pageCount > 2) {
    PdfPage page2;
    const uint32_t pageObjId = pageTree.getPageObjectId(2);
    REQF(pageObjId != 0);
    PdfFixedString<PDF_OBJECT_BODY_MAX> pageBodyFixed;
    REQF(xref.readDictForObject(file, pageObjId, pageBodyFixed));
    std::string pageBody(pageBodyFixed.view());
    REQF(parseSinglePage(file, xref, pageBody, page2));
    dumpPageText(2, page2);
  }

  std::printf("OK  %s  pages=%u  outline=%zu  page0_blocks=%zu  page0_chars~%zu\n", base,
              static_cast<unsigned>(pageCount), outline.size(), page0.textBlocks.size(), totalTextChars(page0));
  printFirstTextBlockPreview("page0 first block", page0);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  testPdfPageNavigationPolicy();
  testPdfCachedPageReader();
  testInflateReaderLongWindowStreaming();
  std::vector<const char*> paths;
  if (argc > 1) {
    for (int i = 1; i < argc; ++i) paths.push_back(argv[i]);
  } else {
    paths = {"test/pdf/sample.pdf", "test/pdf/EE-366.pdf", "test/pdf/esp32-c6_datasheet_en.pdf",
             "test/pdf/Problem-Solving Treatment_ Learning and Pl - IHS.pdf"};
  }

  for (const char* path : paths) {
    runOnePdf(path);
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "\n%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("\nAll PDF parser checks passed.\n");
  return 0;
}
