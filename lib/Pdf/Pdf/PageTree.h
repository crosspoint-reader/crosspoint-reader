#pragma once

#include <HalStorage.h>

#include "PdfFixed.h"
#include "PdfLimits.h"
#include "XrefTable.h"

class PageTree {
  PdfFixedVector<uint32_t, PDF_MAX_PAGES> pageOffsets;
  PdfFixedVector<uint32_t, PDF_MAX_PAGES> pageObjectIds;
  uint16_t pageIndexByObjectId_[PDF_MAX_OBJECTS]{};
  bool pageIndexMapReady_ = false;
  static constexpr uint16_t kInvalidPageIndex = 0xFFFF;

 public:
  bool parse(FsFile& file, const XrefTable& xref, uint32_t rootObjId);
  bool setPageObjectIds(const PdfFixedVector<uint32_t, PDF_MAX_PAGES>& pageObjectIds);
  const PdfFixedVector<uint32_t, PDF_MAX_PAGES>& objectIds() const { return pageObjectIds; }
  uint32_t pageCount() const;
  uint32_t getPageOffset(uint32_t pageIndex) const;
  uint32_t getPageObjectId(uint32_t pageIndex) const;
  uint32_t pageIndexForObjectId(uint32_t objId) const;
};
