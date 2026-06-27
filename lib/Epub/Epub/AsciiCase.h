#pragma once

#include <cstdint>

namespace epub {

// ASCII-only lowercase fold: 'A'-'Z' -> 'a'-'z'; every other byte (including
// multi-byte UTF-8 sequences) is returned unchanged. constexpr so it stays in
// flash and folds at compile time. Shared by the CSS keyword matcher and the
// in-book search matcher so the logic has a single definition.
constexpr uint8_t asciiToLower(const uint8_t value) {
  return (value >= 'A' && value <= 'Z') ? static_cast<uint8_t>(value + ('a' - 'A')) : value;
}

}  // namespace epub
