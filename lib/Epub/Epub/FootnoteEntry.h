#pragma once

struct FootnoteEntry {
  char number[3];
  char href[64];
  bool isInline;

  FootnoteEntry() : isInline(false) {
    number[0] = '\0';
    href[0] = '\0';
  }
};
