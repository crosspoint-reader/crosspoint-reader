#pragma once

#include <string>

#include "Epub.h"

namespace serialization {

inline void writeString(HalFile&, const std::string&) {}
inline bool tryReadString(HalFile&, std::string& out, size_t) {
  out.clear();
  return true;
}

}  // namespace serialization
