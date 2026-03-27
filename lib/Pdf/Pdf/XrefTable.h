#pragma once

#include <HalStorage.h>

#include "PdfFixed.h"
#include "PdfLimits.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

class XrefTable {
  // Packed little-endian 24-bit offsets: 3 bytes per object.
  uint8_t offsets_[PDF_MAX_OBJECTS * 3]{};
  uint32_t offsetCount_ = 0;
  uint32_t rootObjId_ = 0;

  struct InlineEntry {
    bool used = false;
    uint32_t objId = 0;
    uint16_t dictLen = 0;
    uint16_t streamLen = 0;
    char dict[PDF_INLINE_DICT_MAX]{};
    uint8_t stream[PDF_INLINE_STREAM_MAX]{};
  };

  InlineEntry inline_[PDF_MAX_INLINE_OBJECTS]{};
  uint8_t objStmContainers_[(PDF_MAX_OBJECTS + 7) / 8]{};
  uint16_t inlineVictim_ = 0;

  bool parseXrefStream(FsFile& file, size_t fileSize, uint32_t xrefObjOffset);
  bool loadObjStreamForTarget(FsFile& file, uint32_t stmObjId, uint32_t targetObjId);

  bool insertInlineObject(uint32_t objNum, const PdfFixedString<PDF_INLINE_DICT_MAX>& d, const uint8_t* stm,
                          size_t stmLen);
  const InlineEntry* findInline(uint32_t objId) const;

 public:
  bool parse(FsFile& file);
  uint32_t getOffset(uint32_t objId) const;
  uint32_t objectCount() const;
  uint32_t rootObjId() const;

  // Used by classic xref merge (non-member in .cpp); bounded xref table updates only.
  bool setOffset(uint32_t objId, uint32_t off);
  void ensureOffsetCount(uint32_t n);

  bool readDictForObject(FsFile& file, uint32_t objId, PdfFixedString<PDF_OBJECT_BODY_MAX>& dictBody) const;
  bool readStreamMetaForObject(FsFile& file, uint32_t objId, PdfFixedString<PDF_OBJECT_BODY_MAX>& dictOut,
                               uint32_t& streamOffset, uint32_t& streamLength, bool& flateDecode) const;
  bool readStreamForObject(FsFile& file, uint32_t objId, PdfFixedString<PDF_OBJECT_BODY_MAX>& dictOut,
                           PdfByteBuffer& streamPayload, bool& flateDecode) const;
};
