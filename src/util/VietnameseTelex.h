#pragma once

#include <cstddef>
#include <string>

namespace VietnameseTelex {

// Applies one ASCII key at a UTF-8 byte cursor. The edit is atomic: text and
// cursor are left unchanged when the cursor is invalid or maxBytes would be
// exceeded. Processing is limited to the current 16-codepoint token.
bool applyKey(std::string& text, size_t& cursorByte, char asciiKey, size_t maxBytes = 0);

// Repositions an existing tone mark after an external edit such as Delete.
bool normalizeAtCursor(std::string& text, size_t& cursorByte, size_t maxBytes = 0);

}  // namespace VietnameseTelex
