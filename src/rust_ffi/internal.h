#pragma once

// Shared by the Rust FFI translation units in this directory.
//
// Each file here implements one concern's `cpp_*` entry points; the Rust side
// declares them in lib/backend_rs/src/raw.rs. Only what more than one file
// needs lives here.

#include <GfxRenderer.h>

#include <cstdint>

class MappedInputManager;
class ActivityRs;

// Bound by ActivityRs at the top of every loop() and render(), so they always
// name the Rust activity that is currently running.
extern GfxRenderer* g_rustRendererPtr;
extern MappedInputManager* g_rustInputPtr;
extern ActivityRs* g_rustActivityPtr;

namespace rust_ffi {

// Rust hands text over as uint8_t* to keep the declarations free of char
// signedness; the bytes are always UTF-8 and null-terminated.
inline const char* asText(const uint8_t* text) { return reinterpret_cast<const char*>(text); }

// Mirrors FontStyle in the Rust font module.
inline EpdFontFamily::Style resolveStyle(const uint8_t style) {
  switch (style) {
    case 1:
      return EpdFontFamily::BOLD;
    case 2:
      return EpdFontFamily::ITALIC;
    case 3:
      return EpdFontFamily::BOLD_ITALIC;
    default:
      return EpdFontFamily::REGULAR;
  }
}

}  // namespace rust_ffi
