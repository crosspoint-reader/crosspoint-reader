#pragma once

#include <cstdint>

#define REPLACEMENT_GLYPH 0xFFFD

int utf8CodepointLen(const unsigned char c);
uint32_t utf8NextCodepoint(const unsigned char** string);
