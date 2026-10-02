// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#include "ZimFold.h"

#include <algorithm>

namespace zim {
namespace {

struct FoldRow {
  uint32_t cp;
  const char* folded;
};

constexpr FoldRow kFoldTable[] = {
#include "ZimFoldTable.inc"
};

const char* lookup(uint32_t cp) {
  const FoldRow* end = kFoldTable + sizeof(kFoldTable) / sizeof(kFoldTable[0]);
  const FoldRow* it = std::lower_bound(kFoldTable, end, cp, [](const FoldRow& r, uint32_t c) { return r.cp < c; });
  return it != end && it->cp == cp ? it->folded : nullptr;
}

// Decodes one UTF-8 sequence at s[i]. Returns its length, or 0 if invalid.
size_t decode(std::string_view s, size_t i, uint32_t& cp) {
  const auto b = [&](size_t k) { return static_cast<uint8_t>(s[i + k]); };
  const uint8_t c = b(0);
  size_t len;
  if (c < 0x80) {
    cp = c;
    return 1;
  } else if ((c & 0xe0) == 0xc0) {
    len = 2;
    cp = c & 0x1f;
  } else if ((c & 0xf0) == 0xe0) {
    len = 3;
    cp = c & 0x0f;
  } else if ((c & 0xf8) == 0xf0) {
    len = 4;
    cp = c & 0x07;
  } else {
    return 0;
  }
  if (i + len > s.size()) return 0;
  for (size_t k = 1; k < len; ++k) {
    if ((b(k) & 0xc0) != 0x80) return 0;
    cp = (cp << 6) | (b(k) & 0x3f);
  }
  return len;
}

bool isSpace(uint32_t cp) { return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v'; }

}  // namespace

std::string foldKey(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool pendingSpace = false;
  const auto emit = [&](std::string_view piece) {
    for (char ch : piece) {
      if (isSpace(static_cast<uint8_t>(ch))) {
        pendingSpace = !out.empty();
        continue;
      }
      if (pendingSpace) {
        out.push_back(' ');
        pendingSpace = false;
      }
      out.push_back(ch);
    }
  };
  for (size_t i = 0; i < text.size();) {
    uint32_t cp = 0;
    const size_t len = decode(text, i, cp);
    if (len == 0) {  // invalid byte: keep it, move on
      emit(text.substr(i, 1));
      ++i;
      continue;
    }
    if (cp < 0x80) {
      const char c = static_cast<char>(cp >= 'A' && cp <= 'Z' ? cp + ('a' - 'A') : cp);
      emit(std::string_view(&c, 1));
    } else if (const char* folded = lookup(cp)) {
      emit(folded);
    } else {
      emit(text.substr(i, len));
    }
    i += len;
  }
  if (out.size() > kMaxKeyBytes) {
    size_t cut = kMaxKeyBytes;
    while (cut > 0 && (static_cast<uint8_t>(out[cut]) & 0xc0) == 0x80) --cut;
    out.resize(cut);
    while (!out.empty() && out.back() == ' ') out.pop_back();
  }
  return out;
}

}  // namespace zim
