#include "Pdf.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "Pdf/ContentStream.h"
#include "Pdf/PdfObject.h"

namespace {

uint32_t contentsObjectId(std::string_view pageBody) {
  PdfFixedString<PDF_DICT_VALUE_MAX> cv;
  if (!PdfObject::getDictValue("/Contents", pageBody, cv)) {
    return 0;
  }
  while (cv.size() > 0 && (cv[0] == ' ' || cv[0] == '\t' || cv[0] == '\r' || cv[0] == '\n')) {
    cv.erase_prefix(1);
  }
  if (cv.empty()) return 0;
  if (cv[0] == '[') {
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

}  // namespace

Pdf::~Pdf() { close(); }

Pdf::Pdf(Pdf&& o) noexcept = default;
Pdf& Pdf::operator=(Pdf&& o) noexcept = default;

void Pdf::close() {
  if (valid_) {
    file_.close();
    valid_ = false;
  }
  pages_ = 0;
  outlineEntries_.clear();
  path_.clear();
}

bool Pdf::open(const char* path) {
  close();
  if (!path || !path[0]) {
    return false;
  }
  if (!path_.assign(path, std::strlen(path))) {
    LOG_ERR("PDF", "Path too long");
    return false;
  }

  if (!Storage.exists(path_.c_str())) {
    LOG_ERR("PDF", "File does not exist: %s", path_.c_str());
    return false;
  }

  if (!Storage.openFileForRead("PDF", path_.c_str(), file_)) {
    LOG_ERR("PDF", "Cannot open: %s", path_.c_str());
    return false;
  }

  if (!xref_.parse(file_)) {
    LOG_ERR("PDF", "Failed to parse xref: %s", path_.c_str());
    file_.close();
    return false;
  }

  PdfFixedString<PDF_OBJECT_BODY_MAX> catalogBody;
  const uint32_t rootId = xref_.rootObjId();
  if (rootId == 0 || !xref_.readDictForObject(file_, rootId, catalogBody)) {
    LOG_ERR("PDF", "Bad catalog");
    file_.close();
    return false;
  }

  const uint32_t pagesObjId = PdfObject::getDictRef("/Pages", catalogBody.view());
  if (pagesObjId == 0 || !pageTree_.parse(file_, xref_, pagesObjId)) {
    LOG_ERR("PDF", "Failed to parse page tree");
    file_.close();
    return false;
  }

  pages_ = pageTree_.pageCount();
  cache_.configure(path_.c_str());

  uint32_t cachedPageCount = 0;
  if (!cache_.loadMeta(cachedPageCount, outlineEntries_) || cachedPageCount != pages_) {
    outlineEntries_.clear();
    const uint32_t outlinesId = PdfObject::getDictRef("/Outlines", catalogBody.view());
    if (outlinesId != 0) {
      PdfOutlineParser::parse(file_, xref_, pageTree_, outlinesId, outlineEntries_);
    }
    cache_.saveMeta(pages_, outlineEntries_);
  }

  valid_ = true;
  return true;
}

bool Pdf::saveProgress(uint32_t page) { return valid_ && cache_.saveProgress(page); }

bool Pdf::loadProgress(uint32_t& page) { return valid_ && cache_.loadProgress(page); }

bool Pdf::getPage(uint32_t pageNum, PdfPage& out) {
  out.clear();
  if (!valid_ || pageNum >= pages_) {
    return false;
  }
  if (cache_.loadPage(pageNum, out)) {
    return true;
  }

  const uint32_t pageObjId = pageTree_.getPageObjectId(pageNum);
  if (pageObjId == 0) {
    return false;
  }

  PdfFixedString<PDF_OBJECT_BODY_MAX> pageBody;
  if (!xref_.readDictForObject(file_, pageObjId, pageBody)) {
    return false;
  }

  const uint32_t contentId = contentsObjectId(pageBody.view());
  if (contentId == 0) {
    LOG_ERR("PDF", "No /Contents for page %u", static_cast<unsigned>(pageNum));
    return false;
  }

  PdfFixedString<PDF_OBJECT_BODY_MAX> contentDict;
  PdfByteBuffer streamPayload;
  bool compressed = false;
  if (!xref_.readStreamForObject(file_, contentId, contentDict, streamPayload, compressed)) {
    return false;
  }
  if (streamPayload.len == 0) {
    return false;
  }

  if (!ContentStream::parseBuffer(streamPayload.ptr(), streamPayload.len, compressed, file_, xref_, pageBody.view(),
                                   out)) {
    LOG_ERR("PDF", "Failed to parse page %u", static_cast<unsigned>(pageNum));
    return false;
  }

  cache_.savePage(pageNum, out);
  return true;
}

const PdfFixedString<PDF_MAX_PATH>& Pdf::cacheDirectory() const { return cache_.getCacheDir(); }

size_t Pdf::extractImageStream(const PdfImageDescriptor& img, uint8_t* outBuf, size_t maxBytes) {
  if (!valid_ || !outBuf || maxBytes == 0 || img.pdfStreamLength == 0) {
    return 0;
  }
  if (!file_.seek(img.pdfStreamOffset)) {
    return 0;
  }
  const size_t toRead = std::min(maxBytes, static_cast<size_t>(img.pdfStreamLength));
  const int r = file_.read(outBuf, toRead);
  if (r <= 0) {
    return 0;
  }
  return static_cast<size_t>(r);
}
