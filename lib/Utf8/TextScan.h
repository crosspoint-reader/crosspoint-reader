#pragma once

#include "IndicScripts.h"

struct Utf8TextScan {
  bool rtl = false;
  bool complexScript = false;
  bool tokenPlanes = false;
};

// Byte-level prechecks shared by bidi resolution and complex-script layout.
inline Utf8TextScan utf8ScanText(const char* text) {
  Utf8TextScan scan;
  if (text == nullptr) return scan;
  for (const auto* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
    if (*p >= 0xD6 && *p <= 0xDB) {
      scan.rtl = true;
    } else if (*p == 0xE0) {
      scan.complexScript = scan.complexScript || indic::startsIndic(p);
    } else if (*p == 0xF4 || (*p == 0xF3 && p[1] >= 0xB0)) {
      scan.tokenPlanes = true;
    }
  }
  return scan;
}
