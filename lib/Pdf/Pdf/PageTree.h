#pragma once

#include <HalStorage.h>

#include "PdfFixed.h"
#include "PdfLimits.h"
#include "XrefTable.h"

class PageTree {
  PdfFixedVector<uint32_t, PDF_MAX_PAGES> pageOffsets;
  PdfFixedVector<uint32_t, PDF_MAX_PAGES> pageObjectIds;

 public:
  bool parse(FsFile& file, const XrefTable& xref, uint32_t rootObjId);
  uint32_t pageCount() const;
  uint32_t getPageOffset(uint32_t pageIndex) const;
  uint32_t getPageObjectId(uint32_t pageIndex) const;
  uint32_t pageIndexForObjectId(uint32_t objId) const;
};
