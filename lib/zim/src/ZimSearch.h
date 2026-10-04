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
// Records read, at most, among those whose key is the query itself (every
// title ending in that word, in a word index), to find the title itself.
constexpr size_t kExactRunLimit = 16384;

struct SearchStats {
  uint32_t recordsRead = 0;
};

// The two halves of an indexed search, for searching several archives at
// once: rank each archive's candidates (index pages only, no directory reads),
// merge them, then read entries only for the results shown.
struct SearchCandidate {
  uint32_t entry = 0;
  uint8_t score = 0;
  bool exact = false;
};
// The records starting with `query`, ranked (exact, score, key): every whole
// title equal to it, the most popular titles that end in it, and up to
// kSearchWindow that go on past it.
Error searchCandidates(TitleIndex& index, TitleIndex::Cursor& cursor, std::string_view query,
                       std::vector<SearchCandidate>& out);
// Reads `c`'s entry into a hit unless its article is in `seenArticles`
// (which it then joins). Returns false for a duplicate.
Error hitFromCandidate(Archive& archive, const SearchCandidate& c, std::vector<uint32_t>& seenArticles, SearchHit& out,
                       bool& added);

// `index` may be null; `cursor` must be non-null when `index` is (one 4 KB
// leaf page, reused across keystrokes). Directory reads: one per result, plus
// one per duplicate (a redirect to an article already listed) skipped.
Error searchTitles(Archive& archive, TitleIndex* index, TitleIndex::Cursor* cursor, std::string_view query, size_t max,
                   std::vector<SearchHit>& out, SearchStats* stats = nullptr);

// Search across several archives at once (the device's "All" scope). Each
// source is searched as above, then merged: every source's exact matches
// first, in source order, then one result from each source in turn, so a small
// collection's best match sits beside the big one's instead of under it
// (popularity scores are not comparable between archives). A source that
// fails is left out; the call fails only if every source did.
struct SearchSource {
  Archive* archive = nullptr;
  TitleIndex* index = nullptr;           // may be null: the archive's own title list
  TitleIndex::Cursor* cursor = nullptr;  // required with an index
};
struct MultiHit {
  size_t source = 0;  // position in `sources`
  SearchHit hit;
};
Error searchMany(const std::vector<SearchSource>& sources, std::string_view query, size_t max,
                 std::vector<MultiHit>& out);

}  // namespace zim
