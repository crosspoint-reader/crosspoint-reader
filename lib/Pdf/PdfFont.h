#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "PdfObject.h"

class PdfDoc;

// One decoded font resource: maps show-string bytes to UTF-8. Primary path is
// the /ToUnicode CMap; simple fonts (Type1/TrueType/Type3) fall back to
// /Encoding (WinAnsi/MacRoman/Standard) plus /Differences via a glyph-name
// table. CID (Type0) fonts without ToUnicode cannot be mapped at all.
class PdfFont {
 public:
  void build(PdfDoc& doc, const PdfObj& fontDict);

  // Decode show-string bytes, appending UTF-8. False when this font cannot
  // map anything (CID without ToUnicode) — caller counts the loss.
  bool decode(const uint8_t* s, size_t len, std::string& out) const;

  size_t codeWidth() const { return codeBytes; }
  bool canMap() const { return hasToUnicode || !isCID; }

  uint32_t objNum = 0;  // cache key; 0 = direct (uncacheable) dict

 private:
  struct MapEntry {
    uint32_t lo = 0;
    uint32_t hi = 0;
    uint32_t dstOfs = 0;  // into dstPool
    uint16_t dstLen = 0;  // UTF-16 code units; last unit += (code - lo)
  };

  void parseToUnicode(const uint8_t* d, size_t len);
  void addEntry(uint32_t lo, uint32_t hi, const std::string& dstBytes);
  bool mapCode(uint32_t code, std::string& out) const;

  std::vector<MapEntry> cmap;
  std::vector<uint16_t> dstPool;
  uint16_t simple[256] = {};  // simple-font byte -> Unicode
  bool hasToUnicode = false;
  bool isCID = false;
  uint8_t codeBytes = 1;
};

// Document-lifetime font cache keyed on the font object number, so fonts
// shared across pages are built (ToUnicode parsed) once.
class PdfFontCache {
 public:
  // fontRef: the value from a resources /Font dict (Ref or inline dict).
  const PdfFont* get(PdfDoc& doc, const PdfObj* fontRef);
  // Call between pages only — invalidates PdfFont pointers when trimming.
  void trim();

 private:
  std::vector<std::unique_ptr<PdfFont>> fonts;
};
