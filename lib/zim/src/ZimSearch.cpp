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

Error searchMany(const std::vector<SearchSource>& sources, std::string_view query, size_t max,
                 std::vector<MultiHit>& out) {
  out.clear();
  if (max == 0 || sources.empty()) return Error::None;

  struct Lane {
    std::vector<SearchCandidate> candidates;  // indexed source
    std::vector<SearchHit> ready;             // source without an index: hits already read
    std::vector<uint32_t> seen;
    size_t next = 0;
    bool dead = false;
  };
  std::vector<Lane> lanes(sources.size());
  Error firstError = Error::None;
  size_t failed = 0;
  for (size_t i = 0; i < sources.size(); i++) {
    const SearchSource& s = sources[i];
    Error err = Error::NoMemory;
    if (s.archive && s.index && s.cursor)
      err = searchCandidates(*s.index, *s.cursor, query, lanes[i].candidates);
    else if (s.archive && !s.index)
      err = searchTitles(*s.archive, nullptr, nullptr, query, max, lanes[i].ready);
    if (err != Error::None) {
      lanes[i].dead = true;
      if (failed++ == 0) firstError = err;
    }
  }

  // The lane's next distinct result, if any (and, with exactOnly, if exact).
  auto take = [&](size_t i, bool exactOnly, SearchHit& hit) {
    Lane& l = lanes[i];
    if (l.dead) return false;
    if (!sources[i].index) {
      if (l.next >= l.ready.size() || (exactOnly && !l.ready[l.next].exact)) return false;
      hit = std::move(l.ready[l.next++]);
      return true;
    }
    while (l.next < l.candidates.size()) {
      const SearchCandidate& c = l.candidates[l.next];
      if (exactOnly && !c.exact) return false;
      l.next++;
      bool added = false;
      if (hitFromCandidate(*sources[i].archive, c, l.seen, hit, added) != Error::None) {
        l.dead = true;
        return false;
      }
      if (added) return true;
    }
    return false;
  };

  SearchHit hit;
  for (size_t i = 0; i < sources.size(); i++) {
    while (out.size() < max && take(i, true, hit)) out.push_back({i, std::move(hit)});
  }
  for (bool progress = true; progress && out.size() < max;) {
    progress = false;
    for (size_t i = 0; i < sources.size() && out.size() < max; i++) {
      if (take(i, false, hit)) {
        out.push_back({i, std::move(hit)});
        progress = true;
      }
    }
  }
  return failed == sources.size() ? firstError : Error::None;
}

}  // namespace zim
