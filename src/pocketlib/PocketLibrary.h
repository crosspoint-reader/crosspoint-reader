// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

// The card's library: what /library/manifest.json (written by the card
// builder) says is there, and its ZIM archives.
//
// Opening an archive reads only its header, MIME list and the start of its
// listings (a few KB of RAM), so archives are opened on first use and then
// stay open: searching every collection at once needs them all. What is
// large is the decoded-cluster cache (two ~2 MiB clusters in PSRAM); only the
// collection being read (the "focused" one, open()) keeps it, the others' are
// dropped when the focus moves.

#include <ZimArchive.h>
#include <ZimTitleIndex.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pocketlib {

struct Collection {
  std::string key;                 // folder under /library, e.g. "wikipedia"
  std::string title;               // "Wikipedia"
  std::string description;         // "The free encyclopedia"
  std::string date;                // "2026-06"
  std::vector<std::string> parts;  // absolute card paths, in order
  uint64_t bytes = 0;
  std::string indexPath;  // .pltitles, may be empty
  std::string group;      // "Medicine": shown together under one tile; may be empty
  std::string iconPath;   // 1-bit BMP from the ZIM's own illustration; may be empty
};

// Timings of the last article opened, for the collection screen and serial log.
struct OpenTimings {
  uint32_t lookupMs = 0;
  uint32_t readMs = 0;
  uint32_t cleanMs = 0;
  uint32_t firstPageMs = 0;
  uint32_t allPagesMs = 0;
  uint32_t htmlBytes = 0;
  uint32_t cleanBytes = 0;
  uint16_t pages = 0;
  bool clusterCached = false;
  bool valid = false;
};

class Library {
 public:
  static Library& instance();

  // Reads /library/manifest.json; falls back to scanning /library/<key>/ for
  // .zim / .zimaa files. Returns the number of collections found.
  size_t load();
  const std::vector<Collection>& collections() const { return collections_; }
  const std::string& loadError() const { return loadError_; }

  // Opens (or returns the already-open) archive for collections()[i] and
  // makes it the focused one: the other archives drop their cluster caches.
  zim::Archive* open(size_t i, zim::Error* error = nullptr);
  // collections()[i]'s title index, or null if it has none / it is stale /
  // the archive is not open.
  zim::TitleIndex* titleIndex(size_t i);
  void close();

  // Titles starting with `query`, best first, at most `max`, redirects to
  // the same article collapsed (zim::searchTitles / zim::searchMany). Uses
  // the card's .pltitles index (case- and accent-insensitive) where there is
  // one, else the ZIM's own byte-ordered title list (case-sensitive). With
  // several collections: exact matches first, then the collections take
  // turns. Collections that cannot be opened are left out.
  struct Hit {
    size_t collection = 0;
    uint32_t entry = 0;  // directory index (may be a redirect; the reader resolves it)
    std::string title;
    bool exact = false;
  };
  bool search(const std::vector<size_t>& scope, std::string_view query, size_t max, std::vector<Hit>& out);
  bool search(size_t collection, std::string_view query, size_t max, std::vector<Hit>& out) {
    return search(std::vector<size_t>{collection}, query, max, out);
  }

  OpenTimings lastOpen;
  uint32_t lastSearchMs = 0;

 private:
  Library() = default;
  bool loadManifest();
  void scanFolders();

  struct Slot {
    std::unique_ptr<zim::Archive> archive;
    std::unique_ptr<zim::TitleIndex> index;
    std::unique_ptr<zim::TitleIndex::Cursor> cursor;  // one 4 KB leaf page, reused per keystroke
    bool failed = false;                              // don't retry a broken collection every keystroke
  };
  // Opens collections()[i] without changing the focus.
  zim::Archive* ensureOpen(size_t i, zim::Error* error);
  void dropOtherCaches(size_t keep);  // keep = SIZE_MAX drops every cache

  std::vector<Collection> collections_;
  std::string loadError_;
  std::vector<Slot> slots_;  // parallel to collections_
  size_t focus_ = SIZE_MAX;
};

// "52.7 GB", "272 MB"
std::string formatBytes(uint64_t bytes);
// "19,191,219"
std::string formatCount(uint32_t n);

}  // namespace pocketlib
