// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#include "ZimSearch.h"

#include <algorithm>

#include "ZimFold.h"

namespace zim {

Error searchCandidates(TitleIndex& index, TitleIndex::Cursor& cursor, std::string_view query,
                       std::vector<SearchCandidate>& out) {
  out.clear();
  const std::string key = foldKey(query);
  if (key.empty()) return Error::None;
  Error err = index.seek(key, cursor);
  if (err != Error::None) return err;
  TitleRecord rec;
  while (out.size() < kSearchWindow) {
    err = index.next(cursor, rec);
    if (err == Error::NotFound) break;
    if (err != Error::None) return err;
    if (!keyHasPrefix(rec.key, key)) break;
    out.push_back({rec.entry, rec.score, rec.key == key});
  }
  // Exact first, then popularity; key order (the scan order) breaks ties.
  std::stable_sort(out.begin(), out.end(), [](const SearchCandidate& a, const SearchCandidate& b) {
    if (a.exact != b.exact) return a.exact;
    return a.score > b.score;
  });
  return Error::None;
}

Error hitFromCandidate(Archive& archive, const SearchCandidate& c, std::vector<uint32_t>& seenArticles, SearchHit& out,
                       bool& added) {
  added = false;
  Entry e;
  const Error err = archive.entryAt(c.entry, e);
  if (err != Error::None) return err;
  const uint32_t target = e.isRedirect() ? e.redirectIndex : e.index;
  if (std::find(seenArticles.begin(), seenArticles.end(), target) != seenArticles.end()) return Error::None;
  seenArticles.push_back(target);
  // A redirect is shown under its article's title, with what matched in
  // brackets: "Global warming (Calentamiento global)".
  std::string title = std::move(e.title);
  if (e.isRedirect()) {
    Entry t;
    if (archive.entryAt(target, t) == Error::None && t.title != title) title = t.title + " (" + title + ")";
  }
  out = {c.entry, std::move(title), c.score, c.exact};
  added = true;
  return Error::None;
}

Error searchTitles(Archive& archive, TitleIndex* index, TitleIndex::Cursor* cursor, std::string_view query, size_t max,
                   std::vector<SearchHit>& out, SearchStats* stats) {
  out.clear();
  if (stats) *stats = {};
  if (max == 0) return Error::None;
  std::vector<uint32_t> seen;

  if (index) {
    if (!cursor) return Error::NoMemory;
    std::vector<SearchCandidate> candidates;
    Error err = searchCandidates(*index, *cursor, query, candidates);
    if (err != Error::None) return err;
    if (stats) stats->recordsRead = static_cast<uint32_t>(candidates.size());
    for (const SearchCandidate& c : candidates) {
      if (out.size() >= max) break;
      SearchHit hit;
      bool added = false;
      err = hitFromCandidate(archive, c, seen, hit, added);
      if (err != Error::None) return err;
      if (added) out.push_back(std::move(hit));
    }
    return Error::None;
  }

  if (query.empty() || !archive.hasTitleIndex()) return Error::None;
  const char ns = archive.contentNamespace();
  uint32_t pos = 0;
  Error err = archive.lowerBoundTitle(ns, query, pos);
  Entry e;
  size_t scanned = 0;
  while (err == Error::None && out.size() < max && scanned < max * 4 && pos < archive.titleCount()) {
    err = archive.titleEntryAt(pos++, e);
    if (err != Error::None || e.ns != ns || e.title.compare(0, query.size(), query) != 0) break;
    scanned++;
    SearchHit hit;
    bool added = false;
    err = hitFromCandidate(archive, {e.index, 0, e.title.size() == query.size()}, seen, hit, added);
    if (added) out.push_back(std::move(hit));
  }
  if (stats) stats->recordsRead = static_cast<uint32_t>(scanned);
  return err;
}

}  // namespace zim
