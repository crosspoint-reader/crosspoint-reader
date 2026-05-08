#pragma once

#include <cstring>

#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 96

struct FootnoteEntry {
  char number[FOOTNOTE_NUMBER_LEN];
  char href[FOOTNOTE_HREF_LEN];

  FootnoteEntry() {
    number[0] = '\0';
    href[0] = '\0';
  }
};
