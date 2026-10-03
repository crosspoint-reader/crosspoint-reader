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
  const size_t maxScan = max * 4;
  size_t scanned = 0;
  std::vector<uint32_t> targets;  // articles already listed

  // Returns false on a read error.
  auto add = [&](uint32_t entryIndex) -> Error {
    Entry e;
    const Error err = archive.entryAt(entryIndex, e);
    if (err != Error::None) return err;
    const uint32_t target = e.isRedirect() ? e.redirectIndex : e.index;
    if (std::find(targets.begin(), targets.end(), target) != targets.end()) return Error::None;
    targets.push_back(target);
    out.push_back({entryIndex, std::move(e.title)});
    return Error::None;
  };

  Error err = Error::None;
  if (index) {
    if (!cursor) return Error::NoMemory;
    const std::string key = foldKey(query);
    if (key.empty()) return Error::None;
    err = index->seek(key, *cursor);
    if (err != Error::None) return err;
    TitleRecord rec;
    while (out.size() < max && scanned < maxScan) {
      err = index->next(*cursor, rec);
      if (err == Error::NotFound) {
        err = Error::None;
        break;
      }
      if (err != Error::None || !keyHasPrefix(rec.key, key)) break;
      scanned++;
      err = add(rec.entry);
      if (err != Error::None) break;
    }
  } else if (!query.empty() && archive.hasTitleIndex()) {
    const char ns = archive.contentNamespace();
    uint32_t pos = 0;
    err = archive.lowerBoundTitle(ns, query, pos);
    Entry e;
    while (err == Error::None && out.size() < max && scanned < maxScan && pos < archive.titleCount()) {
      err = archive.titleEntryAt(pos++, e);
      if (err != Error::None || e.ns != ns || e.title.compare(0, query.size(), query) != 0) break;
      scanned++;
      err = add(e.index);
    }
  }
  if (stats) stats->recordsRead = static_cast<uint32_t>(scanned);
  return err;
}

}  // namespace zim
