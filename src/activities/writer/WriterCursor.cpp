#include "WriterCursor.h"

#include <Utf8.h>

namespace {

size_t previousCodepointStart(const std::string& text, size_t cursor) {
  size_t pos = cursor - 1;
  // Don't stop on continuation bytes
  while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) {
    --pos;
  }
  return pos;
}

uint32_t codepointAt(const std::string& text, size_t cursor) {
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(text.data() + cursor);
  return utf8NextCodepoint(&ptr);
}

}  // namespace

size_t WriterCursor::clamp(const std::string& text, size_t cursor) {
  if (cursor >= text.size()) {
    return text.size();
  }

  while (cursor > 0 && (static_cast<unsigned char>(text[cursor]) & 0xC0) == 0x80) {
    --cursor;
  }
  return cursor;
}

size_t WriterCursor::moveLeft(const std::string& text, size_t cursor) {
  cursor = clamp(text, cursor);
  if (cursor == 0) {
    return 0;
  }

  size_t previous = previousCodepointStart(text, cursor);
  while (previous > 0 && utf8IsCombiningMark(codepointAt(text, previous))) {
    previous = previousCodepointStart(text, previous);
  }
  return previous;
}

size_t WriterCursor::moveRight(const std::string& text, size_t cursor) {
  cursor = clamp(text, cursor);
  if (cursor >= text.size()) {
    return text.size();
  }

  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(text.data() + cursor);
  utf8NextCodepoint(&ptr);

  const unsigned char* textEnd = reinterpret_cast<const unsigned char*>(text.data() + text.size());
  while (ptr < textEnd) {
    const unsigned char* next = ptr;
    if (!utf8IsCombiningMark(utf8NextCodepoint(&next))) {
      break;
    }
    ptr = next;
  }

  return static_cast<size_t>(ptr - reinterpret_cast<const unsigned char*>(text.data()));
}
