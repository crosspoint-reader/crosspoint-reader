#pragma once

struct FootnoteEntry {
  char number[24];
  char href[64];
  bool isInline;

  FootnoteEntry() : isInline(false) {
    number[0] = '\0';
    href[0] = '\0';
  }
};
