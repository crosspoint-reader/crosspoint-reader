#pragma once
// Minimal host-side stand-in for the Arduino String class.
//
// FsHelpers.h includes <WString.h> for its String-overloaded extension helpers,
// but the FAT32 sanitizer under test is plain C-string code. This shim provides
// just enough of the Arduino String surface (construction, c_str(), length())
// for the header to parse on the host so the unit tests can link FsHelpers.cpp
// without pulling in the Arduino core.
#include <cstddef>
#include <string>

class String {
 public:
  String() = default;
  String(const char* s) : value_(s ? s : "") {}
  String(const std::string& s) : value_(s) {}

  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.length(); }

 private:
  std::string value_;
};
