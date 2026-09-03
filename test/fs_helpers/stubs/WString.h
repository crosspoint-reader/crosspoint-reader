#pragma once

// Minimal stand-in for Arduino's WString.h, just enough to satisfy FsHelpers.h's
// `const String&` overloads when FsHelpers.cpp is compiled for native tests.

#include <string>

class String {
 public:
  String() = default;
  String(const char* s) : data_(s ? s : "") {}

  const char* c_str() const { return data_.c_str(); }
  size_t length() const { return data_.length(); }

 private:
  std::string data_;
};
