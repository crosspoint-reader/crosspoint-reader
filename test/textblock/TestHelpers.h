// Shared TextBlock construction helpers for the host test binaries.
//
// Both targets build the block under test the same way. Keeping one definition
// here means the behavioural tests and the allocation tests cannot drift on how
// a TextBlock is constructed -- the allocation counts are only comparable to the
// behavioural coverage if both are measuring the same shape of object.

#pragma once

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

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

// Ruby annotations are the only thing TextBlock draws at SUP, so counting SUP
// draws counts annotations.
inline int countSupCalls(const GfxRenderer& renderer) {
  int n = 0;
  for (const auto& call : renderer.textCalls) {
    if ((call.style & EpdFontFamily::SUP) != 0) n++;
  }
  return n;
}

// Assert two renders emitted the same text draws. Used to pin "these two blocks
// render identically" without restating the absolute geometry, so the check
// survives a change to the stub's synthetic metrics.
inline void expectSameTextCalls(const GfxRenderer& a, const GfxRenderer& b) {
  ASSERT_EQ(a.textCalls.size(), b.textCalls.size());
  for (size_t i = 0; i < a.textCalls.size(); i++) {
    SCOPED_TRACE("draw call " + std::to_string(i));
    EXPECT_EQ(a.textCalls[i].text, b.textCalls[i].text);
    EXPECT_EQ(a.textCalls[i].x, b.textCalls[i].x);
    EXPECT_EQ(a.textCalls[i].y, b.textCalls[i].y);
    EXPECT_EQ(a.textCalls[i].style, b.textCalls[i].style);
  }
}
