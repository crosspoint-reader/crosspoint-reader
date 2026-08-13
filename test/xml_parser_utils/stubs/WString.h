#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

// Host-test stand-in for Arduino's String (WString.h). Only the members used by
// the compiled firmware headers (FsHelpers.h inline helpers, HalStorage.h
// declarations) are provided.

class String {
 public:
  String() = default;
  String(const char* s) : data_(s ? s : "") {}
  String(const std::string& s) : data_(s) {}
  String(std::string&& s) : data_(std::move(s)) {}

  const char* c_str() const { return data_.c_str(); }
  size_t length() const { return data_.size(); }
  bool isEmpty() const { return data_.empty(); }

  bool operator==(const String& other) const { return data_ == other.data_; }
  bool operator==(const char* s) const { return data_ == (s ? s : ""); }

 private:
  std::string data_;
};
