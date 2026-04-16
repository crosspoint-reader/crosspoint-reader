#include "Pdf.h"

#include <HalStorage.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "Pdf/ContentStream.h"
#include "Pdf/PdfLog.h"
#include "Pdf/PdfObject.h"

namespace {

constexpr size_t kSourceSignatureChunkSize = 64;

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

struct CatalogInfo {
  uint32_t pagesObjId = 0;
  uint32_t outlinesId = 0;
  uint32_t namesObjId = 0;
};

uint32_t hashBytes(const uint8_t* data, size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    hash ^= static_cast<uint32_t>(data[i]);
    hash *= 16777619u;
  }
  return hash;
}

bool readSignatureChunk(FsFile& file, size_t offset, size_t maxLen, uint32_t& outHash) {
  uint8_t buf[kSourceSignatureChunkSize]{};
  if (!file.seek(offset)) {
    return false;
  }

  const size_t toRead = std::min(maxLen, sizeof(buf));
  const int read = file.read(buf, toRead);
  if (read < 0) {
    return false;
  }

  outHash = hashBytes(buf, static_cast<size_t>(read));
  return true;
}

bool loadCatalogInfo(FsFile& file, const XrefTable& xref, uint32_t rootId, CatalogInfo& info) {
  static PdfFixedString<PDF_OBJECT_BODY_MAX> catalogBody;
  info = {};
  if (rootId == 0 || !xref.readDictForObject(file, rootId, catalogBody)) {
    return false;
  }
  info.pagesObjId = PdfObject::getDictRef("/Pages", catalogBody.view());
  info.outlinesId = PdfObject::getDictRef("/Outlines", catalogBody.view());
  info.namesObjId = PdfObject::getDictRef("/Names", catalogBody.view());
  return info.pagesObjId != 0;
}

}  // namespace

Pdf::~Pdf() { close(); }

Pdf::Pdf(Pdf&& o) noexcept = default;
Pdf& Pdf::operator=(Pdf&& o) noexcept = default;

void Pdf::close() {
  if (file_.isOpen()) {
    file_.close();
  }
  file_ = FsFile();
  valid_ = false;
  xrefReady_ = false;
  outlinesFromCache_ = false;
  pageMapFromCache_ = false;
  metaSaved_ = false;
  pages_ = 0;
  outlineEntries_.clear();
  cachedPageObjectIds_.clear();
  cachedSourceSignature_ = {};
  sourceSignature_ = {};
  path_.clear();
  streamScratch_.clear();
}

bool Pdf::computeSourceSignature(SourceSignature& outSignature) {
  outSignature = {};
  const size_t fileSize = file_.fileSize();
  if (fileSize == 0) {
    return false;
  }

  outSignature.fileSize = fileSize;
  const size_t headLen = std::min(fileSize, kSourceSignatureChunkSize);
  if (!readSignatureChunk(file_, 0, headLen, outSignature.headHash)) {
    return false;
  }

  const size_t tailOffset = (fileSize > kSourceSignatureChunkSize) ? (fileSize - kSourceSignatureChunkSize) : 0;
  const size_t tailLen = std::min(fileSize - tailOffset, kSourceSignatureChunkSize);
  if (!readSignatureChunk(file_, tailOffset, tailLen, outSignature.tailHash)) {
    return false;
  }

  return true;
}

bool Pdf::parseFromSource(bool needsOutlines) {
  CatalogInfo catalogInfo;
  if (!xref_.parse(file_)) {
    return false;
  }
  if (!loadCatalogInfo(file_, xref_, xref_.rootObjId(), catalogInfo)) {
    return false;
  }
  if (!pageTree_.parse(file_, xref_, catalogInfo.pagesObjId)) {
    return false;
  }

  pages_ = pageTree_.pageCount();
  cachedPageObjectIds_.clear();
  for (uint32_t i = 0; i < pages_; ++i) {
    if (!cachedPageObjectIds_.push_back(pageTree_.getPageObjectId(i))) {
      return false;
    }
  }

  if (needsOutlines) {
    outlineEntries_.clear();
    if (catalogInfo.outlinesId != 0) {
      PdfOutlineParser::parse(file_, xref_, pageTree_, catalogInfo.outlinesId, catalogInfo.namesObjId, outlineEntries_);
    }
  }

  cachedSourceSignature_ = sourceSignature_;
  xrefReady_ = true;
  pageMapFromCache_ = false;
  outlinesFromCache_ = false;
  metaSaved_ = false;
  return true;
}

bool Pdf::ensureXrefReady() {
  if (xrefReady_) {
    return true;
  }
  if (!valid_) {
    return false;
  }
  if (!xref_.parse(file_)) {
    return false;
  }
  xrefReady_ = true;
  return true;
}

bool Pdf::open(const char* path) {
  close();
  if (!path || !path[0]) {
    return false;
  }
  if (!path_.assign(path, std::strlen(path))) {
    pdfLogErr("Path too long");
    return false;
  }

  if (!Storage.openFileForRead("PDF", path_.c_str(), file_)) {
    pdfLogErrPath("Cannot open: ", path_.c_str());
    return false;
  }

  cache_.configure(path_.c_str(), file_.fileSize());
  if (!computeSourceSignature(sourceSignature_)) {
    pdfLogErrPath("Failed to fingerprint source: ", path_.c_str());
    file_.close();
    return false;
  }

  uint32_t cachedPageCount = 0;
  uint32_t cachedFileSize = 0;
  uint32_t cachedHead = 0;
  uint32_t cachedTail = 0;
  if (cache_.loadMeta(cachedPageCount, outlineEntries_, &cachedPageObjectIds_, &cachedFileSize, &cachedHead,
                      &cachedTail) &&
      cachedPageCount > 0 && cachedPageCount <= PDF_MAX_PAGES && cachedPageCount == cachedPageObjectIds_.size() &&
      cachedFileSize == static_cast<uint32_t>(sourceSignature_.fileSize) && cachedHead == sourceSignature_.headHash &&
      cachedTail == sourceSignature_.tailHash && cache_.allPagesCached(cachedPageCount) &&
      pageTree_.setPageObjectIds(cachedPageObjectIds_)) {
    pages_ = cachedPageCount;
    cachedSourceSignature_ = sourceSignature_;
    outlinesFromCache_ = true;
    pageMapFromCache_ = true;
    xrefReady_ = false;
    metaSaved_ = true;
    valid_ = true;
    return true;
  }

  if (!parseFromSource(true)) {
    pdfLogErrPath("Failed to parse PDF source: ", path_.c_str());
    file_.close();
    return false;
  }

  valid_ = true;
  return true;
}

bool Pdf::saveProgress(uint32_t page) { return valid_ && cache_.saveProgress(page); }

bool Pdf::loadProgress(uint32_t& page) { return valid_ && cache_.loadProgress(page); }

bool Pdf::getPage(uint32_t pageNum, PdfPage& out) {
  static PdfFixedString<PDF_OBJECT_BODY_MAX> pageBody;
  static PdfFixedString<PDF_OBJECT_BODY_MAX> contentDict;
  out.clear();
  if (!valid_ || pageNum >= pages_) {
    return false;
  }
  if (cache_.loadPage(pageNum, out)) {
    return true;
  }

  if (!ensureXrefReady()) {
    return false;
  }

  const uint32_t pageObjId = pageTree_.getPageObjectId(pageNum);
  if (pageObjId == 0) {
    return false;
  }

  if (!xref_.readDictForObject(file_, pageObjId, pageBody)) {
    return false;
  }

  const uint32_t contentId = contentsObjectId(pageBody.view());
  if (contentId == 0) {
    pdfLogErr("No /Contents for page");
    return false;
  }

  uint32_t streamOffset = 0;
  uint32_t streamLength = 0;
  bool compressed = false;
  if (!xref_.readStreamMetaForObject(file_, contentId, contentDict, streamOffset, streamLength, compressed)) {
    if (!xref_.readStreamForObject(file_, contentId, contentDict, streamScratch_, compressed)) {
      return false;
    }
    if (streamScratch_.len == 0) {
      return false;
    }
    if (!ContentStream::parseBuffer(streamScratch_.ptr(), streamScratch_.len, compressed, file_, xref_, pageBody.view(),
                                    out)) {
      pdfLogErr("Failed to parse page");
      return false;
    }
  } else {
    if (!ContentStream::parse(file_, streamOffset, streamLength, compressed, xref_, pageBody.view(), out)) {
      pdfLogErr("Failed to parse page");
      return false;
    }
  }

  cache_.savePage(pageNum, out);
  if (!metaSaved_) {
    persistCacheMetaIfNeeded();
  }
  return true;
}

bool Pdf::persistCacheMetaIfNeeded() {
  if (metaSaved_) {
    return true;
  }
  if (!valid_) {
    return false;
  }
  if (!cache_.saveMeta(pages_, outlineEntries_, &cachedPageObjectIds_, static_cast<uint32_t>(sourceSignature_.fileSize),
                       sourceSignature_.headHash, sourceSignature_.tailHash)) {
    return false;
  }
  metaSaved_ = true;
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

size_t Pdf::extractImageStreamToFile(const PdfImageDescriptor& img, FsFile& outFile, size_t maxBytes) {
  if (!valid_ || maxBytes == 0 || img.pdfStreamLength == 0) {
    return 0;
  }
  if (!file_.seek(img.pdfStreamOffset)) {
    return 0;
  }

  uint8_t chunk[512];
  size_t remaining = std::min(maxBytes, static_cast<size_t>(img.pdfStreamLength));
  size_t copied = 0;
  while (remaining > 0) {
    const size_t toRead = std::min(remaining, sizeof(chunk));
    const int r = file_.read(chunk, toRead);
    if (r <= 0) {
      break;
    }

    const size_t readLen = static_cast<size_t>(r);
    if (outFile.write(chunk, readLen) != readLen) {
      break;
    }
    copied += readLen;
    remaining -= readLen;
  }
  return copied;
}
