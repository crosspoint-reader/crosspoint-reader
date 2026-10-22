#pragma once

#include <cmath>

// Sized variants of the reader font (built-in, installed .cpfont, or vector).
// resolve returns a registered font id, or 0 when unavailable; load re-registers
// a cached id after eviction or reboot, returning false when unavailable.
struct FontVariantProvider {
  void* ctx = nullptr;
  int (*resolve)(void* ctx, int fontId, float scale) = nullptr;
  bool (*load)(void* ctx, int fontId) = nullptr;

  int resolveFont(const int fontId, const float scale) const {
    // Keep small CSS size adjustments from selecting a different font size.
    if (std::fabs(scale - 1.0f) < 0.05f) return fontId;
    return resolve ? resolve(ctx, fontId, scale) : 0;
  }
};
