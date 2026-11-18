#pragma once

#include <Print.h>

struct GifTestDisplay {
  int getDisplayWidth() const { return 64; }
  int getDisplayHeight() const { return 40; }
};

inline GifTestDisplay display;
