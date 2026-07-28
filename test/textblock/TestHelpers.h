// Shared TextBlock construction helpers for the host test binaries.
//
// Both targets build the block under test the same way. Keeping one definition
// here means the behavioural tests and the allocation tests cannot drift on how
// a TextBlock is constructed -- the allocation counts are only comparable to the
// behavioural coverage if both are measuring the same shape of object.

#pragma once

#include <EpdFontFamily.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Epub/blocks/TextBlock.h"

using Style = EpdFontFamily::Style;

// Build a TextBlock from words at explicit x positions, with no focus-reading
// annotations. `ruby` may be empty (the common, ruby-less case).
inline std::unique_ptr<TextBlock> makeBlock(const std::vector<std::string>& words, const std::vector<int16_t>& xpos,
                                            const std::vector<Style>& styles,
                                            const std::vector<std::string>& ruby = {}) {
  return std::make_unique<TextBlock>(words, xpos, styles, std::vector<uint8_t>{}, std::vector<uint16_t>{}, BlockStyle(),
                                     ruby);
}

inline std::vector<Style> regularStyles(const size_t n) { return std::vector<Style>(n, EpdFontFamily::REGULAR); }
