#pragma once

#include <string>

#include "Epub.h"

namespace serialization {

inline void writeString(HalFile&, const std::string&) {}
inline void readString(HalFile&, std::string& out) { out.clear(); }

}  // namespace serialization
