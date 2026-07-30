#include "PdfToEpub.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <MinizConfig.h>
#include <Logging.h>
#include <Memory.h>
#include <ZipWriter.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "PdfDoc.h"
#include "PdfFont.h"
#include "PdfImage.h"
#include "PdfText.h"
#include "PdfUtil.h"

namespace {

constexpr int PAGES_PER_CHAPTER = 10;
// Bump whenever the emitted EPUB changes shape, so cached conversions from an
// older firmware are regenerated instead of reused. 2 = images.
constexpr uint32_t CONVERTER_VERSION = 2;
constexpr size_t MAX_IMAGES = 2000;

// PDF text string -> UTF-8: UTF-16BE with BOM, else PDFDocEncoding (~latin1).
std::string pdfTextString(const std::string& s) {
  std::string out;
  if (s.size() >= 2 && (uint8_t)s[0] == 0xFE && (uint8_t)s[1] == 0xFF) {
    for (size_t i = 2; i + 1 < s.size(); i += 2) {
      const uint16_t unit = (uint16_t)(((uint8_t)s[i] << 8) | (uint8_t)s[i + 1]);
      pdfUtf16ToUtf8(&unit, 1, out);  // ponytail: surrogate pairs split across calls land as U+FFFD
    }
  } else {
    for (const char c : s) {
      const uint8_t u = (uint8_t)c;
      if (u >= 0x20) pdfAppendUtf8(out, u);
    }
  }
  if (out.size() > 256) {
    // Trim back off any continuation bytes so the cut never lands mid-sequence
    // (a split code point would make the OPF malformed XML).
    size_t end = 256;
    while (end > 0 && ((uint8_t)out[end] & 0xC0) == 0x80) end--;
    out.resize(end);
  }
  return out;
}

bool hasReadableChar(const std::string& s) {
  for (const char c : s) {
    if ((uint8_t)c > 0x20) return true;
  }
  return false;
}

void xmlEscapeInto(std::string& out, const std::string& s) { pdfXmlEscapeAppend(out, s.data(), s.size()); }

// Image sink for PdfText: stores each unique image XObject in the EPUB once and
// appends an <img> reference wherever the content stream draws it. Matches the
// markup lib/Mobi/MobiToEpub.cpp emits, which the reader resolves correctly.
struct ImageEmitter {
  struct Entry {
    uint32_t objNum;  // 0 = not an indirect object; never matched for reuse
    uint16_t idx;
    bool jpeg;
  };

  ZipWriter* zip = nullptr;
  std::string* chapter = nullptr;
  std::vector<Entry> entries;
  uint32_t onPage = 0;  // references appended since the caller last zeroed it
  bool truncated = false;

  static void trampoline(void* ctx, PdfDoc& doc, const PdfObj& img, uint32_t objNum, const PdfObj* res) {
    static_cast<ImageEmitter*>(ctx)->emit(doc, img, objNum, res);
  }

  void emit(PdfDoc& doc, const PdfObj& img, uint32_t objNum, const PdfObj* res) {
    if (objNum) {
      // ponytail: linear reuse scan, bounded by MAX_IMAGES; a map would cost
      // more heap than the compares are worth at conversion time.
      for (const auto& e : entries) {
        if (e.objNum == objNum) {
          reference(e);
          return;
        }
      }
    }
    if (entries.size() >= MAX_IMAGES) {
      if (!truncated) {
        truncated = true;
        LOG_ERR("PDF", "image cap of %u reached, later images dropped", (unsigned)MAX_IMAGES);
      }
      return;
    }
    PdfImage::Decoded dec;
    if (!PdfImage::decode(doc, img, res, dec)) return;  // decode() logged the reason

    const uint16_t idx = (uint16_t)entries.size();
    const bool jpeg = dec.isJpeg();
    const uint8_t* bytes = jpeg ? dec.jpeg.data() : dec.png.data();
    const size_t len = jpeg ? dec.jpeg.size() : dec.png.size();
    char name[48];
    snprintf(name, sizeof(name), "OEBPS/images/img%04u.%s", (unsigned)idx, jpeg ? "jpg" : "png");
    if (!zip->addFile(name, bytes, len)) {
      LOG_ERR("PDF", "image %u: EPUB write failed", (unsigned)idx);
      return;
    }
    LOG_INF("PDF", "image %u: %ux%u %s, %u bytes", (unsigned)idx, (unsigned)dec.width, (unsigned)dec.height,
            jpeg ? "jpg" : "png", (unsigned)len);
    entries.push_back({objNum, idx, jpeg});
    reference(entries.back());
  }

  void reference(const Entry& e) {
    char tag[112];
    snprintf(tag, sizeof(tag), "<p style=\"text-align:center\"><img src=\"images/img%04u.%s\" alt=\"\"/></p>\n",
             (unsigned)e.idx, e.jpeg ? "jpg" : "png");
    *chapter += tag;
    onPage++;
  }
};

}  // namespace

std::string PdfToEpub::ensureConverted(const std::string& pdfPath, std::string* errorOut) {
  auto setError = [&](const std::string& e) {
    if (errorOut) *errorOut = e;
    LOG_ERR("PDF", "%s", e.c_str());
  };

  // Cache: keyed on path hash; validated on source file size.
  const uint32_t hash = (uint32_t)std::hash<std::string>{}(pdfPath);
  char cacheDir[48];
  snprintf(cacheDir, sizeof(cacheDir), "/.crosspoint/pdf_%08x", (unsigned)hash);
  const std::string epubPath = std::string(cacheDir) + "/book.epub";
  const std::string tagPath = std::string(cacheDir) + "/src.bin";

  uint32_t curSize = 0;
  uint32_t curFingerprint = 0;
  {
    HalFile f;
    if (!Storage.openFileForRead("PDF", pdfPath, f)) {
      setError("cannot open PDF file");
      return "";
    }
    curSize = (uint32_t)f.fileSize();
    curFingerprint = FsHelpers::sourceFingerprint(f, curSize);
  }
  {
    HalFile f;
    uint32_t srcTag[4] = {0, 0, 0, 0};
    if (Storage.openFileForRead("PDF", tagPath, f)) {
      // Size alone can't tell two different files apart, and the cache
      // directory is keyed on a path hash, so a same-size replacement (or a
      // hash collision) would otherwise be served the previous book.
      if (f.read(srcTag, sizeof(srcTag)) == (int)sizeof(srcTag) && srcTag[0] == CONVERTER_VERSION &&
          srcTag[1] == curSize && srcTag[2] == curFingerprint && Storage.exists(epubPath.c_str())) {
        return epubPath;  // valid cached conversion
      }
    }
  }

  PdfDoc doc;
  std::string userErr;
  if (!doc.open(pdfPath, &userErr)) {
    setError(userErr);
    return "";
  }
  const uint32_t pageCount = (uint32_t)doc.pages().size();
  LOG_INF("PDF", "converting %s (%u pages)", pdfPath.c_str(), (unsigned)pageCount);
  Storage.ensureDirectoryExists(cacheDir);

  auto zip = makeUniqueNoThrow<ZipWriter>();
  if (!zip || !zip->begin(epubPath.c_str())) {
    setError("cannot write EPUB");
    return "";
  }
  // EPUB structural entries; mimetype MUST be first (all entries are stored).
  static const char MIMETYPE[] = "application/epub+zip";
  static const char CONTAINER[] =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
      "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
      "</rootfiles>\n</container>\n";
  if (!zip->addFile("mimetype", (const uint8_t*)MIMETYPE, sizeof(MIMETYPE) - 1) ||
      !zip->addFile("META-INF/container.xml", (const uint8_t*)CONTAINER, sizeof(CONTAINER) - 1)) {
    setError("cannot write EPUB");
    return "";
  }

  // Chapters: 10 PDF pages each, extracted and written one at a time so peak
  // RAM stays at one chapter of text.
  PdfFontCache fonts;
  const uint32_t chapterCount = (pageCount + PAGES_PER_CHAPTER - 1) / PAGES_PER_CHAPTER;
  bool anyText = false;
  bool writeFailed = false;
  std::string chapterBuf;
  ImageEmitter imgs;
  imgs.zip = zip.get();
  imgs.chapter = &chapterBuf;
  imgs.entries.reserve(32);
  PdfImage::resetSkipLog();
  const PdfText::ImageSink sink{&imgs, &ImageEmitter::trampoline};
  for (uint32_t ch = 0; ch < chapterCount && !writeFailed; ch++) {
    chapterBuf.clear();
    chapterBuf +=
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n<head><title>c</title></head>\n<body>\n";
    const uint32_t firstPage = ch * PAGES_PER_CHAPTER;
    const uint32_t lastPage = firstPage + PAGES_PER_CHAPTER < pageCount ? firstPage + PAGES_PER_CHAPTER : pageCount;
    for (uint32_t pg = firstPage; pg < lastPage; pg++) {
      bool pageAny = false;
      imgs.onPage = 0;
      PdfText::extractPage(doc, doc.pages()[pg], fonts, chapterBuf, &pageAny, &sink);
      if (pageAny) anyText = true;
      if (!pageAny && imgs.onPage == 0) {  // truly empty page: leave a placeholder
        char marker[32];
        snprintf(marker, sizeof(marker), "<p>[page %u]</p>\n", (unsigned)(pg + 1));
        chapterBuf += marker;
      }
    }
    chapterBuf += "</body>\n</html>\n";
    char name[40];
    snprintf(name, sizeof(name), "OEBPS/chapter%04u.xhtml", (unsigned)ch);
    if (!zip->addFile(name, (const uint8_t*)chapterBuf.data(), chapterBuf.size())) writeFailed = true;
    vTaskDelay(1);  // yield: keep the watchdog + render task happy on big PDFs
  }

  // A document with no text but usable images is a scanned/comic PDF, and those
  // images are renderable — ship the image-only book. Only a document that
  // yields neither is a dead end.
  if (writeFailed || (!anyText && imgs.entries.empty())) {
    zip.reset();  // close the output before removing it
    Storage.remove(epubPath.c_str());
    setError(writeFailed ? "EPUB write failed (SD full?)" : "no extractable text or images (scanned PDF?)");
    return "";
  }

  // Title/author from /Info when readable, else the filename.
  std::string title, author;
  {
    PdfObj store;
    if (const PdfObj* t = doc.resolve(doc.info().find("Title"), store); t && t->kind == PdfObj::Kind::String) {
      const std::string decoded = pdfTextString(t->str);
      if (hasReadableChar(decoded)) title = decoded;
    }
    if (const PdfObj* a = doc.resolve(doc.info().find("Author"), store); a && a->kind == PdfObj::Kind::String) {
      const std::string decoded = pdfTextString(a->str);
      if (hasReadableChar(decoded)) author = decoded;
    }
  }
  if (title.empty()) {
    const size_t slash = pdfPath.find_last_of('/');
    title = slash == std::string::npos ? pdfPath : pdfPath.substr(slash + 1);
    if (title.size() > 4) {
      std::string ext = title.substr(title.size() - 4);
      for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
      if (ext == ".pdf") title.resize(title.size() - 4);
    }
  }

  // Manifest + spine.
  std::string opf;
  opf.reserve(4096);
  opf +=
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"bookid\" version=\"2.0\">\n"
      "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf\">\n";
  opf += "<dc:title>";
  xmlEscapeInto(opf, title);
  opf += "</dc:title>\n<dc:creator>";
  xmlEscapeInto(opf, author);
  opf += "</dc:creator>\n<dc:language>en</dc:language>\n<dc:identifier id=\"bookid\">pdf-";
  {
    char hex[12];
    snprintf(hex, sizeof(hex), "%08x", (unsigned)hash);
    opf += hex;
  }
  opf += "</dc:identifier>\n</metadata>\n<manifest>\n";
  opf += "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n";
  for (uint32_t ch = 0; ch < chapterCount; ch++) {
    char line[128];
    snprintf(line, sizeof(line),
             "<item id=\"ch%04u\" href=\"chapter%04u.xhtml\" media-type=\"application/xhtml+xml\"/>\n", (unsigned)ch,
             (unsigned)ch);
    opf += line;
  }
  for (const auto& e : imgs.entries) {
    char line[160];
    snprintf(line, sizeof(line), "<item id=\"img%04u\" href=\"images/img%04u.%s\" media-type=\"image/%s\"/>\n",
             (unsigned)e.idx, (unsigned)e.idx, e.jpeg ? "jpg" : "png", e.jpeg ? "jpeg" : "png");
    opf += line;
  }
  opf += "</manifest>\n<spine toc=\"ncx\">\n";
  for (uint32_t ch = 0; ch < chapterCount; ch++) {
    char line[64];
    snprintf(line, sizeof(line), "<itemref idref=\"ch%04u\"/>\n", (unsigned)ch);
    opf += line;
  }
  opf += "</spine>\n</package>\n";
  if (!zip->addFile("OEBPS/content.opf", (const uint8_t*)opf.data(), opf.size())) {
    setError("EPUB write failed");
    return "";
  }

  // TOC: one entry per 10-page chapter.
  std::string ncx;
  ncx.reserve(2048);
  ncx +=
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n<head/>\n<docTitle><text>";
  xmlEscapeInto(ncx, title);
  ncx += "</text></docTitle>\n<navMap>\n";
  for (uint32_t ch = 0; ch < chapterCount; ch++) {
    const uint32_t first = ch * PAGES_PER_CHAPTER + 1;
    const uint32_t last = first + PAGES_PER_CHAPTER - 1 < pageCount ? first + PAGES_PER_CHAPTER - 1 : pageCount;
    char line[256];
    snprintf(line, sizeof(line),
             "<navPoint id=\"n%04u\" playOrder=\"%u\"><navLabel><text>Pages %u\xE2\x80\x93%u</text></navLabel>"
             "<content src=\"chapter%04u.xhtml\"/></navPoint>\n",
             (unsigned)ch, (unsigned)(ch + 1), (unsigned)first, (unsigned)last, (unsigned)ch);
    ncx += line;
  }
  ncx += "</navMap>\n</ncx>\n";
  if (!zip->addFile("OEBPS/toc.ncx", (const uint8_t*)ncx.data(), ncx.size()) || !zip->finish()) {
    setError("EPUB write failed");
    return "";
  }

  // Cache tag: {converter version, source file size, page count}.
  {
    HalFile f;
    if (Storage.openFileForWrite("PDF", tagPath.c_str(), f)) {
      const uint32_t tag[4] = {CONVERTER_VERSION, curSize, curFingerprint, pageCount};
      f.write(tag, sizeof(tag));
    }
  }
  LOG_INF("PDF", "converted: %u pages -> %u chapters, %u images", (unsigned)pageCount, (unsigned)chapterCount,
          (unsigned)imgs.entries.size());
  return epubPath;
}
