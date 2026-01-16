#include "OpdsStream.h"

OpdsParserStream::OpdsParserStream(OpdsParser& parser) : parser(parser) {}

int OpdsParserStream::available() { return 0; }

int OpdsParserStream::peek() { abort(); }

int OpdsParserStream::read() { abort(); }

size_t OpdsParserStream::write(uint8_t c) {
  parser.push(reinterpret_cast<const char*>(&c), 1);
  return 1;
}

size_t OpdsParserStream::write(const uint8_t* buffer, size_t size) {
  parser.push(reinterpret_cast<const char*>(buffer), size);
  return size;
}

OpdsParserStream::~OpdsParserStream() { parser.finish(); }
