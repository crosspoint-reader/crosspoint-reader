// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

#include <string_view>

namespace library {

// Where a title files on the shelf: its fold without a leading "The", "A" or
// "An", the way a library shelves "The Road" under R. Only the sort and the
// letter groups use it; search still matches the whole title, so typing
// "the road" finds it. "A Is for Alibi" keeps its A: there the A is a letter,
// not an article. Takes and returns folded text (lower case, single spaces).
inline std::string_view titleSortKey(const std::string_view folded) {
  for (const std::string_view article : {std::string_view("the "), std::string_view("an "), std::string_view("a ")}) {
    if (folded.size() <= article.size() || folded.compare(0, article.size(), article) != 0) continue;
    const std::string_view rest = folded.substr(article.size());
    if (article == "a " && (rest == "is" || rest.compare(0, 3, "is ") == 0)) return folded;
    return rest;
  }
  return folded;
}

}  // namespace library
