// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

// Compiled only into Pocket Library builds; stock envs see an empty unit.
#ifdef POCKET_LIBRARY

#include "ReadingHistory.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdlib>

namespace pocketlib {
namespace {
constexpr const char* kDir = "/.pocketlib";
constexpr const char* kPath = "/.pocketlib/history.tsv";
constexpr const char* kTmpPath = "/.pocketlib/history.tsv.tmp";
constexpr size_t kMaxFileBytes = 64 * 1024;
constexpr const char* kSearchesPath = "/.pocketlib/searches.txt";
constexpr const char* kSearchesTmpPath = "/.pocketlib/searches.txt.tmp";

std::string clean(const std::string& s) {
  std::string out = s;
  for (char& c : out)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return out;
}
}  // namespace

ReadingHistory& ReadingHistory::instance() {
  static ReadingHistory history;
  return history;
}

const std::vector<Place>& ReadingHistory::places() {
  load();
  return places_;
}

void ReadingHistory::load() {
  if (loaded_) return;
  loaded_ = true;
  HalFile f;
  if (!Storage.openFileForRead("PLIB", kPath, f)) return;
  const size_t size = f.fileSize();
  if (size == 0 || size > kMaxFileBytes) return;
  std::string text(size, '\0');
  if (f.read(text.data(), size) != static_cast<int>(size)) return;
  // collection \t ns \t path \t offset \t title \n
  size_t pos = 0;
  while (pos < text.size() && places_.size() < kMaxPlaces) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    const std::string line = text.substr(pos, eol - pos);
    pos = eol + 1;
    std::string fields[5];
    size_t start = 0;
    int n = 0;
    for (; n < 5; n++) {
      const size_t tab = n < 4 ? line.find('\t', start) : std::string::npos;
      fields[n] = line.substr(start, tab == std::string::npos ? std::string::npos : tab - start);
      if (tab == std::string::npos) break;
      start = tab + 1;
    }
    if (n < 4 || fields[0].empty() || fields[1].size() != 1 || fields[2].empty()) continue;
    Place p;
    p.collection = fields[0];
    p.ns = fields[1][0];
    p.path = fields[2];
    p.offset = static_cast<uint32_t>(strtoul(fields[3].c_str(), nullptr, 10));
    p.title = fields[4].empty() ? p.path : fields[4];
    places_.push_back(std::move(p));
  }
}

void ReadingHistory::save() const {
  Storage.ensureDirectoryExists(kDir);
  std::string text;
  for (const auto& p : places_) {
    text += clean(p.collection);
    text += '\t';
    text += p.ns;
    text += '\t';
    text += clean(p.path);
    text += '\t';
    text += std::to_string(p.offset);
    text += '\t';
    text += clean(p.title);
    text += '\n';
  }
  HalFile f;
  if (!Storage.openFileForWrite("PLIB", kTmpPath, f)) return;
  const bool ok = f.write(reinterpret_cast<const uint8_t*>(text.data()), text.size()) == text.size();
  f.flush();
  f.close();
  // Write-then-rename, so a power cut mid-write never loses the whole list.
  if (ok) {
    Storage.remove(kPath);
    Storage.rename(kTmpPath, kPath);
  }
}

bool ReadingHistory::find(const std::string& collection, char ns, const std::string& path, uint32_t& offset) {
  load();
  for (const auto& p : places_) {
    if (p.ns == ns && p.path == path && p.collection == collection) {
      offset = p.offset;
      return true;
    }
  }
  return false;
}

void ReadingHistory::record(const Place& place) {
  load();
  for (auto it = places_.begin(); it != places_.end(); ++it) {
    if (it->ns == place.ns && it->path == place.path && it->collection == place.collection) {
      if (it == places_.begin() && it->offset == place.offset) return;  // nothing new to save
      places_.erase(it);
      break;
    }
  }
  places_.insert(places_.begin(), place);
  if (places_.size() > kMaxPlaces) places_.resize(kMaxPlaces);
  save();
}

const std::vector<std::string>& ReadingHistory::searches() {
  if (searchesLoaded_) return searches_;
  searchesLoaded_ = true;
  HalFile f;
  if (!Storage.openFileForRead("PLIB", kSearchesPath, f)) return searches_;
  const size_t size = f.fileSize();
  if (size == 0 || size > 4096) return searches_;
  std::string text(size, '\0');
  if (f.read(text.data(), size) != static_cast<int>(size)) return searches_;
  size_t pos = 0;
  while (pos < text.size() && searches_.size() < kMaxSearches) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    if (eol > pos) searches_.push_back(text.substr(pos, eol - pos));
    pos = eol + 1;
  }
  return searches_;
}

void ReadingHistory::recordSearch(const std::string& query) {
  const std::string q = clean(query);
  if (q.empty()) return;
  searches();
  auto it = std::find(searches_.begin(), searches_.end(), q);
  if (it == searches_.begin() && it != searches_.end()) return;
  if (it != searches_.end()) searches_.erase(it);
  searches_.insert(searches_.begin(), q);
  if (searches_.size() > kMaxSearches) searches_.resize(kMaxSearches);
  Storage.ensureDirectoryExists(kDir);
  std::string text;
  for (const auto& s : searches_) text += s + "\n";
  HalFile f;
  if (!Storage.openFileForWrite("PLIB", kSearchesTmpPath, f)) return;
  const bool ok = f.write(reinterpret_cast<const uint8_t*>(text.data()), text.size()) == text.size();
  f.flush();
  f.close();
  if (ok) {
    Storage.remove(kSearchesPath);
    Storage.rename(kSearchesTmpPath, kSearchesPath);
  }
}

}  // namespace pocketlib

#endif  // POCKET_LIBRARY
