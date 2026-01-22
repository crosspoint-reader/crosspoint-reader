#pragma once

#include <Stream.h>

#include "OpdsParser.h"

class OpdsParserStream : public Stream {
 public:
  explicit OpdsParserStream(OpdsParser& parser);

  // That functions are not implimented for that stream
  int available() override;
  int peek() override;
  int read() override;

  virtual size_t write(uint8_t c) override;
  virtual size_t write(const uint8_t* buffer, size_t size) override;

  ~OpdsParserStream() override;

 private:
  OpdsParser& parser;
};
