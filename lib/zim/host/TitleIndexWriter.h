// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

// Builds a title index file (format in src/ZimTitleIndex.h) on the Mac.
// Keys are stored in one arena so English Wikipedia's ~19 M titles fit in
// well under a gigabyte of memory.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ZimArchive.h"

namespace zim {

class TitleIndexWriter {
 public:
  // `key` must already be folded (foldKey) and at most kMaxKeyBytes long.
  // Returns the record's number, for setScore.
  // `word`: the title from a later word on (TitleRecord::word).
  size_t add(std::string_view key, uint32_t entry, uint8_t score = 0, bool word = false);
  void setScore(size_t record, uint8_t score) { records_[record].score = score; }
  size_t size() const { return records_.size(); }

  // Sorts and writes the index to `path` (via a temporary file renamed into
  // place). Returns false and sets `error` on failure. With popularMax, an
  // index of more than twice that many records also gets a popular tree of
  // at most popularMax records: those of the best-scored titles.
  bool write(const std::string& path, const uint8_t zimUuid[16], uint32_t zimEntryCount, std::string* error,
             size_t popularMax = 0);
  // Records in the popular tree of the last write (0: none).
  size_t popularRecords() const { return popularRecords_; }
  // The lowest score whose records, with every higher score's, number at
  // most maxRecords (255 when even the top score has more).
  uint8_t popularThreshold(size_t maxRecords) const;

 private:
  struct Record {
    uint64_t offset;
    uint32_t entry;
    uint8_t length;
    uint8_t score;
    uint8_t flags;  // bit 0: word record
  };
  std::string_view keyOf(const Record& r) const { return std::string_view(arena_.data() + r.offset, r.length); }

  std::vector<char> arena_;
  std::vector<Record> records_;
  size_t popularRecords_ = 0;
};

// Adds every searchable title of `archive`: the front-article list (articles
// and the redirects to them) when the file has one, else every entry in the
// content namespace that is HTML or a redirect. Each title is scored by how
// many titles lead to its article (itself and every redirect to it), on a
// log scale: major topics gather redirects ("Zijin Cheng", "Purple Forbidden
// City"), so the count stands in for popularity, which a ZIM does not record. Walks the directory in file
// order, so it reads sequentially. `progress` (may be null) is called now and
// then with entries visited so far and the total.
Error collectTitles(Archive& archive, TitleIndexWriter& writer, void (*progress)(uint32_t done, uint32_t total));

// Score for an article reached by `titles` titles (see collectTitles).
uint8_t popularityScore(uint32_t titles);

}  // namespace zim
