#pragma once

#include <cstdint>
#include <cstring>

struct LinkEntry {
  char href[256];
  int16_t x, y, w, h;

  LinkEntry() : x(0), y(0), w(0), h(0) { href[0] = '\0'; }
};
