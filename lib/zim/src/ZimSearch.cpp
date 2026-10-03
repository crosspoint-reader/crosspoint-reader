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

Error searchTitles(Archive& archive, TitleIndex* index, TitleIndex::Cursor* cursor, std::string_view query, size_t max,
                   std::vector<SearchHit>& out, SearchStats* stats) {
  out.clear();
  if (stats) *stats = {};
  if (max == 0) return Error::None;
  std::vector<uint32_t> targets;  // articles already listed

  // Reads the entry and lists it unless its article is already listed.
  auto add = [&](uint32_t entryIndex, uint8_t score, bool exact) -> Error {
    Entry e;
    const Error err = archive.entryAt(entryIndex, e);
    if (err != Error::None) return err;
    const uint32_t target = e.isRedirect() ? e.redirectIndex : e.index;
    if (std::find(targets.begin(), targets.end(), target) != targets.end()) return Error::None;
    targets.push_back(target);
    // A redirect is shown under its article's title, with what matched in
    // brackets: "Global warming (Calentamiento global)".
    std::string title = std::move(e.title);
    if (e.isRedirect()) {
      Entry t;
      if (archive.entryAt(target, t) == Error::None && t.title != title) title = t.title + " (" + title + ")";
    }
    out.push_back({entryIndex, std::move(title), score, exact});
    return Error::None;
  };

  if (index) {
    if (!cursor) return Error::NoMemory;
    const std::string key = foldKey(query);
    if (key.empty()) return Error::None;
    Error err = index->seek(key, *cursor);
    if (err != Error::None) return err;

    // Candidates: the first kSearchWindow records with the prefix, in key
    // order. Only keys and scores; no directory reads yet.
    struct Candidate {
      uint32_t entry;
      uint8_t score;
      bool exact;
    };
    std::vector<Candidate> candidates;
    TitleRecord rec;
    while (candidates.size() < kSearchWindow) {
      err = index->next(*cursor, rec);
      if (err == Error::NotFound) {
        err = Error::None;
        break;
      }
      if (err != Error::None) return err;
      if (!keyHasPrefix(rec.key, key)) break;
      candidates.push_back({rec.entry, rec.score, rec.key == key});
    }
    if (stats) stats->recordsRead = static_cast<uint32_t>(candidates.size());
    // Exact first, then popularity; key order (the scan order) breaks ties.
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
      if (a.exact != b.exact) return a.exact;
      return a.score > b.score;
    });
    for (const Candidate& c : candidates) {
      if (out.size() >= max) break;
      err = add(c.entry, c.score, c.exact);
      if (err != Error::None) return err;
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
    err = add(e.index, 0, e.title.size() == query.size());
  }
  if (stats) stats->recordsRead = static_cast<uint32_t>(scanned);
  return err;
}

}  // namespace zim
