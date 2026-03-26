#pragma once

#include <Pdf/PdfLimits.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <utility>

// Fixed-capacity string: no heap. Used throughout the PDF pipeline (NASA-style bounded memory).
template <size_t N>
struct PdfFixedString {
  char buf[N]{};
  size_t len = 0;

  void clear() {
    len = 0;
    if (N > 0) buf[0] = '\0';
  }

  [[nodiscard]] bool empty() const { return len == 0; }
  [[nodiscard]] size_t size() const { return len; }
  [[nodiscard]] const char* c_str() const { return buf; }
  [[nodiscard]] char* data() { return buf; }
  [[nodiscard]] const char* data() const { return buf; }
  [[nodiscard]] std::string_view view() const { return std::string_view(buf, len); }

  [[nodiscard]] char operator[](size_t i) const { return buf[i]; }
  [[nodiscard]] char& operator[](size_t i) { return buf[i]; }

  bool assign(const char* s, size_t n) {
    if (n >= N) return false;
    std::memcpy(buf, s, n);
    len = n;
    buf[len] = '\0';
    return true;
  }

  bool assign(std::string_view v) { return assign(v.data(), v.size()); }

  template <size_t M>
  bool assignFrom(const PdfFixedString<M>& o) {
    return assign(o.data(), o.size());
  }

  bool append(char c) {
    if (len + 1 >= N) return false;
    buf[len++] = c;
    buf[len] = '\0';
    return true;
  }

  bool append(const char* s, size_t n) {
    if (len + n >= N) return false;
    std::memcpy(buf + len, s, n);
    len += n;
    buf[len] = '\0';
    return true;
  }

  bool append(std::string_view v) { return append(v.data(), v.size()); }

  void erase_prefix(size_t n) {
    if (n >= len) {
      clear();
      return;
    }
    std::memmove(buf, buf + n, len - n);
    len -= n;
    buf[len] = '\0';
  }

  size_t find(char ch, size_t pos = 0) const {
    for (size_t i = pos; i < len; ++i) {
      if (buf[i] == ch) return i;
    }
    return std::string_view::npos;
  }

  size_t rfind(const char* needle, size_t needleLen) const {
    if (needleLen == 0 || needleLen > len) return std::string_view::npos;
    for (size_t i = len - needleLen;;) {
      if (std::memcmp(buf + i, needle, needleLen) == 0) return i;
      if (i == 0) break;
      --i;
    }
    return std::string_view::npos;
  }

  bool resize(size_t newLen) {
    if (newLen >= N) return false;
    len = newLen;
    buf[len] = '\0';
    return true;
  }
};

template <typename T, size_t N>
struct PdfFixedVector {
  std::array<T, N> data{};
  size_t count = 0;

  void clear() { count = 0; }

  [[nodiscard]] bool empty() const { return count == 0; }
  [[nodiscard]] size_t size() const { return count; }

  T& operator[](size_t i) { return data[i]; }
  const T& operator[](size_t i) const { return data[i]; }

  T* begin() { return data.data(); }
  T* end() { return data.data() + count; }
  const T* begin() const { return data.data(); }
  const T* end() const { return data.data() + count; }

  const T& back() const { return data[count - 1]; }
  T& back() { return data[count - 1]; }

  bool push_back(const T& v) {
    if (count >= N) return false;
    data[count++] = v;
    return true;
  }

  bool push_back(T&& v) {
    if (count >= N) return false;
    data[count++] = std::move(v);
    return true;
  }

  void pop_back() {
    if (count > 0) --count;
  }

  bool resize(size_t n) {
    if (n > N) return false;
    count = n;
    return true;
  }
};

// Uncompressed stream payload for a single PDF object (same cap as page content stream).
struct PdfByteBuffer {
  std::array<uint8_t, PDF_CONTENT_STREAM_MAX> data{};
  size_t len = 0;

  void clear() { len = 0; }

  [[nodiscard]] bool resize(size_t n) {
    if (n > data.size()) return false;
    len = n;
    return true;
  }

  [[nodiscard]] uint8_t* ptr() { return data.data(); }
  [[nodiscard]] const uint8_t* ptr() const { return data.data(); }
};
