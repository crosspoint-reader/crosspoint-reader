#pragma once

#include "Stream.h"
#include "WString.h"

class StreamString : public Stream {
private:
  String buffer;
  size_t position;

public:
  StreamString() : position(0) {}
  explicit StreamString(const String &str) : buffer(str), position(0) {}

  virtual size_t write(uint8_t c) override {
    buffer += (char)c;
    return 1;
  }

  virtual size_t write(const uint8_t *buf, size_t size) override {
    for (size_t i = 0; i < size; ++i) {
      buffer += (char)buf[i];
    }
    return size;
  }

  virtual int available() override { return buffer.length() - position; }

  virtual int read() override {
    if (position < buffer.length()) {
      return buffer.charAt(position++);
    }
    return -1;
  }

  virtual int peek() override {
    if (position < buffer.length()) {
      return buffer.charAt(position);
    }
    return -1;
  }

  virtual void flush() override {
    // No-op for string stream
  }

  const String &str() const { return buffer; }

  const char *c_str() const { return buffer.c_str(); }

  void clear() {
    buffer = "";
    position = 0;
  }
};