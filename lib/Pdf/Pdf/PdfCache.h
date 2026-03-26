#pragma once

#include "PdfFixed.h"
#include "PdfLimits.h"
#include "PdfOutline.h"
#include "PdfPage.h"

class PdfCache {
  PdfFixedString<PDF_MAX_PATH> cacheDir;

 public:
  PdfCache() = default;
  explicit PdfCache(const char* pdfFilePath) { configure(pdfFilePath); }

  void configure(const char* pdfFilePath, size_t fileSize = 0);

  bool loadMeta(uint32_t& pageCount, PdfFixedVector<PdfOutlineEntry, PDF_MAX_OUTLINE_ENTRIES>& outline);
  bool saveMeta(uint32_t pageCount, const PdfFixedVector<PdfOutlineEntry, PDF_MAX_OUTLINE_ENTRIES>& outline);
  bool loadPage(uint32_t pageNum, PdfPage& outPage);
  bool savePage(uint32_t pageNum, const PdfPage& page);
  bool loadProgress(uint32_t& currentPage);
  bool saveProgress(uint32_t currentPage);
  void invalidate();
  const PdfFixedString<PDF_MAX_PATH>& getCacheDir() const { return cacheDir; }
};
