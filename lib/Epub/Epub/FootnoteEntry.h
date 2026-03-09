#pragma once

#include <cstring>

struct FootnoteEntry {
  char number[64];   // Link display text (e.g. "[1]", "See Chapter 5", or full sentence fragment)
  char href[256];    // Target href — long enough for deep relative EPUB paths and fragment identifiers

  FootnoteEntry() {
    number[0] = '\0';
    href[0] = '\0';
  }
};
