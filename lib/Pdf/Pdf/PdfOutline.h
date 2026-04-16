#pragma once

#include <HalStorage.h>

#include "PdfFixed.h"
#include "PdfLimits.h"
#include "XrefTable.h"

class PageTree;

struct PdfOutlineEntry {
  PdfFixedString<PDF_MAX_OUTLINE_TITLE_BYTES> title;
  uint32_t pageNum = 0;
};

class PdfOutlineParser {
 public:
  static bool parse(FsFile& file, const XrefTable& xref, const PageTree& pageTree, uint32_t outlinesObjId,
                    uint32_t namesObjId, PdfFixedVector<PdfOutlineEntry, PDF_MAX_OUTLINE_ENTRIES>& outEntries);
};
