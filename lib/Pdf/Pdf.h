#pragma once

#include <HalStorage.h>

#include <cstddef>

#include "Pdf/PageTree.h"
#include "Pdf/PdfCache.h"
#include "Pdf/PdfFixed.h"
#include "Pdf/PdfLimits.h"
#include "Pdf/PdfOutline.h"
#include "Pdf/PdfPage.h"
#include "Pdf/XrefTable.h"

class Pdf {
 public:
  Pdf() = default;
  ~Pdf();
  Pdf(Pdf&& o) noexcept;
  Pdf& operator=(Pdf&& o) noexcept;

  Pdf(const Pdf&) = delete;
  Pdf& operator=(const Pdf&) = delete;

  bool open(const char* path);
  void close();

  uint32_t pageCount() const { return valid_ ? pages_ : 0; }
  const PdfFixedVector<PdfOutlineEntry, PDF_MAX_OUTLINE_ENTRIES>& outline() const { return outlineEntries_; }
  const PdfFixedString<PDF_MAX_PATH>& filePath() const { return path_; }

  bool getPage(uint32_t pageNum, PdfPage& out);
  bool saveProgress(uint32_t page);
  bool loadProgress(uint32_t& page);

  size_t extractImageStream(const PdfImageDescriptor& img, uint8_t* outBuf, size_t maxBytes);

  const PdfFixedString<PDF_MAX_PATH>& cacheDirectory() const;

  struct SourceSignature {
    size_t fileSize = 0;
    uint32_t headHash = 0;
    uint32_t tailHash = 0;
  };

 private:
  bool parseFromSource(bool needsOutlines);
  bool ensureXrefReady();
  bool computeSourceSignature(SourceSignature& outSignature);
  bool persistCacheMetaIfNeeded();

  PdfFixedString<PDF_MAX_PATH> path_;
  FsFile file_;
  XrefTable xref_;
  PageTree pageTree_;
  PdfFixedVector<PdfOutlineEntry, PDF_MAX_OUTLINE_ENTRIES> outlineEntries_;
  PdfCache cache_;
  PdfByteBuffer streamScratch_;
  PdfFixedVector<uint32_t, PDF_MAX_PAGES> cachedPageObjectIds_;
  SourceSignature cachedSourceSignature_;
  uint32_t pages_ = 0;
  SourceSignature sourceSignature_;
  bool valid_ = false;
  bool xrefReady_ = false;
  bool outlinesFromCache_ = false;
  bool pageMapFromCache_ = false;
  bool metaSaved_ = false;
};
