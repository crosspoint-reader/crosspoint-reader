// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

// Where each recently read article was left, most recent first, in
// /.pocketlib/history.tsv. Positions are character offsets into the article's
// text (the layout engine's visible-text offset), not page numbers, so they
// survive a change of font, size or margins. The same list is the shelf's
// "Recent" screen.

#include <cstdint>
#include <string>
#include <vector>

namespace pocketlib {

struct Place {
  std::string collection;  // Collection::key
  char ns = 'C';
  std::string path;
  std::string title;
  uint32_t offset = 0;
};

class ReadingHistory {
 public:
  static ReadingHistory& instance();
  const std::vector<Place>& places();
  // Saved offset for this article, if it was read before.
  bool find(const std::string& collection, char ns, const std::string& path, uint32_t& offset);
  // Moves (or adds) the article to the front and saves the file.
  void record(const Place& place);

  static constexpr size_t kMaxPlaces = 50;

  // What was searched for, newest first (a search counts once something
  // from its results is opened).
  const std::vector<std::string>& searches();
  void recordSearch(const std::string& query);
  static constexpr size_t kMaxSearches = 6;

  // Open articles in outline (the lead and the section headings) first.
  bool outlineByDefault();
  void setOutlineByDefault(bool on);

 private:
  ReadingHistory() = default;
  void load();
  void save() const;
  bool loaded_ = false;
  std::vector<Place> places_;
  bool searchesLoaded_ = false;
  int outline_ = -1;  // -1 = not read yet
  std::vector<std::string> searches_;
};

}  // namespace pocketlib
