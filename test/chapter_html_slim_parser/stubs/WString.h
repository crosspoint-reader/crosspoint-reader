#pragma once

// Minimal host-test stand-in for Arduino's WString.h. Only the surface used by
// FsHelpers.h's inline String overloads (c_str/length) is needed; those
// overloads are never called by the chapter parser test (no <img> fixtures),
// but the header must still compile since ChapterHtmlSlimParser.cpp includes
// <FsHelpers.h> unconditionally.

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
