#pragma once

#include <cstddef>
#include <string>

// Masking rule for password text fields, kept as a pure function so it can be
// exercised on the host: KeyboardEntryActivity::displayTextForCurrentState()
// owns the surrounding state (which InputType this is, whether the user has
// toggled visibility, where the cursor sits) and delegates the transform here.
//
// Every byte becomes '*' except the one at revealPos, which is left visible so
// the character just typed can be confirmed. Pass std::string::npos to reveal
// nothing. revealPos is a byte offset, matching the caller's cursor bookkeeping.
//
// Takes its argument by value so the caller can move an existing buffer in and
// the masking happens in place -- displayTextForCurrentState() already owns a
// copy, and a render-path helper should not add a second allocation to it.
inline std::string maskPasswordText(std::string text, const size_t revealPos) {
  for (size_t i = 0; i < text.length(); i++) {
    if (i != revealPos) {
      text[i] = '*';
    }
  }
  return text;
}
