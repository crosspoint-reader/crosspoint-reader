#pragma once

#include <HalStorage.h>

#include "PdfFixed.h"
#include "PdfLimits.h"

#include <string>
#include <string_view>

class XrefTable;

// Minimal PDF object reader (no heap): bounded buffers only.
class PdfObject {
 public:
  static bool readAt(FsFile& file, uint32_t offset, PdfFixedString<PDF_OBJECT_BODY_MAX>& bodyStr,
                     uint32_t* streamOffset = nullptr, uint32_t* streamLength = nullptr,
                     const XrefTable* xrefForIndirectLength = nullptr);

  static bool getDictValue(const char* key, std::string_view dict, PdfFixedString<PDF_DICT_VALUE_MAX>& out);
  static bool getDictValue(const char* key, const std::string& dict, PdfFixedString<PDF_DICT_VALUE_MAX>& out) {
    return getDictValue(key, std::string_view(dict), out);
  }
  static int getDictInt(const char* key, std::string_view dict, int defaultVal = 0);
  static uint32_t getDictRef(const char* key, std::string_view dict);
};
