#pragma once
#include <cctype>
#include <cstring>
#include <string>

class String {
 public:
  String() = default;
  String(const char* s) : data_(s ? s : "") {}
  String(const String& other) = default;
  String& operator=(const String& other) = default;

  unsigned int length() const { return static_cast<unsigned int>(data_.size()); }
  const char* c_str() const { return data_.c_str(); }

  void toLowerCase() {
    for (auto& c : data_) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }

  bool endsWith(const String& suffix) const {
    if (suffix.data_.size() > data_.size()) return false;
    return data_.compare(data_.size() - suffix.data_.size(), suffix.data_.size(), suffix.data_) == 0;
  }

 private:
  std::string data_;
};
