#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Tagged PDF object model (ISO 32000-1 §7.3). No RTTI, no exceptions: a Kind
// enum plus the member set below. Heavy members (str/arr/dict) stay empty for
// kinds that don't use them. Copyable on purpose — page-tree inheritance and
// resource dicts are shared by value.
struct PdfObj {
  enum class Kind : uint8_t { Null, Bool, Int, Real, Name, String, Array, Dict, Ref, Stream };

  Kind kind = Kind::Null;
  bool boolVal = false;
  double num = 0;                                    // Int + Real
  std::string str;                                   // Name (no slash) / String bytes
  std::vector<PdfObj> arr;                           // Array elements
  std::vector<std::pair<std::string, PdfObj>> dict;  // Dict + Stream dict entries
  uint32_t ref = 0;                                  // Ref: object number
  uint16_t gen = 0;                                  // Ref: generation
  uint32_t streamOfs = 0;                            // Stream: absolute file offset of data
  uint32_t streamLen = 0;                            // Stream: raw (encoded) byte count

  bool isNum() const { return kind == Kind::Int || kind == Kind::Real; }
  bool isDict() const { return kind == Kind::Dict || kind == Kind::Stream; }
  int32_t asInt() const { return (int32_t)num; }
  bool isName(const char* n) const { return kind == Kind::Name && str == n; }

  // Dict lookup; nullptr when absent or when this is not a dict/stream.
  const PdfObj* find(const char* key) const {
    if (!isDict()) return nullptr;
    for (const auto& kv : dict) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }

  void reset() { *this = PdfObj(); }
};
