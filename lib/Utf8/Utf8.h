#pragma once

#include <cstdint>
#include <string>
#define REPLACEMENT_GLYPH 0xFFFD

uint32_t utf8NextCodepoint(const unsigned char** string);
// Like utf8NextCodepoint, but substitutes f-ligature sequences (fi, fl, ff, ffi, ffl)
// with their Unicode ligature codepoints (U+FB00–FB04).
uint32_t utf8NextCodepointWithLigatures(const unsigned char** string);
// Remove the last UTF-8 codepoint from a std::string and return the new size.
size_t utf8RemoveLastChar(std::string& str);
// Truncate string by removing N UTF-8 codepoints from the end.
void utf8TruncateChars(std::string& str, size_t numChars);
