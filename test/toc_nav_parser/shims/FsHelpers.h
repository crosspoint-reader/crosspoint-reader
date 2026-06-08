#pragma once

// Host stand-in for <FsHelpers.h> (the real one pulls in Arduino String). The
// nav parser only uses these two helpers for href post-processing; identity
// implementations keep the test focused on the nav-filtering logic under test.
#include <string>

namespace FsHelpers {
inline std::string decodeUriEscapes(const std::string& s) { return s; }
inline std::string normalisePath(const std::string& s) { return s; }
}  // namespace FsHelpers
