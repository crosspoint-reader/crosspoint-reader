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
inline std::string maskPasswordText(const std::string& text, const size_t revealPos) {
  std::string masked = text;
  for (size_t i = 0; i < masked.length(); i++) {
    if (i != revealPos) {
      masked[i] = '*';
    }
  }
  return masked;
}
