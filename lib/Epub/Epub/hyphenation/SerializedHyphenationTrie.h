#pragma once

#include <cstddef>
#include <cstdint>

// Lightweight descriptor that points at a serialized Liang hyphenation trie stored in flash.
struct SerializedHyphenationPatterns {
  const std::uint8_t* data;
  size_t size;
};
