#include "OpdsStream.h"

OpdsParserStream::OpdsParserStream(OpdsParser& parser) : parser(parser) {}

int OpdsParserStream::available() { return 0; }

int OpdsParserStream::peek() { abort(); }

int OpdsParserStream::read() { abort(); }

size_t OpdsParserStream::write(uint8_t c) { return parser.write(c); }

size_t OpdsParserStream::write(const uint8_t* buffer, size_t size) { return parser.write(buffer, size); }

OpdsParserStream::~OpdsParserStream() { parser.flush(); }
