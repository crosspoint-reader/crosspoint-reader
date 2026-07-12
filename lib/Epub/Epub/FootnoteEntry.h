#pragma once

#include <cstring>

struct FootnoteEntry {
  char number[24];
  char href[64];

  FootnoteEntry() {
    number[0] = '\0';
    href[0] = '\0';
  }
};
