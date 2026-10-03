// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

// Title search as you type: the titles that start with what was typed.
//
// With the card's sidecar index (.pltitles) the match ignores case, accents
// and extra spaces; without one it falls back to the ZIM's own title list,
// which is ordered byte by byte and so matches case-sensitively. With a
// version 2 index, the titles are ranked: an exact match first, then by the
// index's popularity score, then alphabetically; up to kSearchWindow matching
// records are considered (two or three index pages), so a one-letter query
// ranks only the start of its range, and each further letter sharpens it.
// Without scores, results come in title order (an exact match is still first). Several redirects to the
// same article ("USA", "U.S.A.", "United States") are listed once, under the
// first title reached, under the article's own title with the matching
// redirect in brackets. Each result costs one directory read (two for a redirect); the number of
// records read is bounded so a one-letter query stays as fast as a long one.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ZimArchive.h"
#include "ZimTitleIndex.h"

namespace zim {

struct SearchHit {
  uint32_t entry = 0;  // directory index; may be a redirect (resolve before reading)
  std::string title;
  uint8_t score = 0;   // popularity from the index (0 without one)
  bool exact = false;  // the folded title equals the folded query
};

constexpr size_t kSearchWindow = 256;

struct SearchStats {
  uint32_t recordsRead = 0;
};

// `index` may be null; `cursor` must be non-null when `index` is (one 4 KB
// leaf page, reused across keystrokes). Directory reads: one per result, plus
// one per duplicate (a redirect to an article already listed) skipped.
Error searchTitles(Archive& archive, TitleIndex* index, TitleIndex::Cursor* cursor, std::string_view query, size_t max,
                   std::vector<SearchHit>& out, SearchStats* stats = nullptr);

}  // namespace zim
