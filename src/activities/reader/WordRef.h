#pragma once

#include <EpdFontFamily.h>

#include <string>

/// A single positioned word on a rendered page, used for text selection activities.
struct WordRef {
  int x, y, w, h;
  int pageIdx;
  std::string text;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  bool paragraphStart = false;
};
