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
// builder) says is there, and the one ZIM archive that is open at a time.
//
// Opening an archive reads only its header, MIME list and the start of its
// listings, so switching collections is cheap; the archive and its title
// index stay open until another collection is opened, so going back and forth
// between the shelf and an article costs nothing.

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

  // Opens (or returns the already-open) archive for collections()[i].
  zim::Archive* open(size_t i, zim::Error* error = nullptr);
  // The open collection's title index, or null if it has none / it is stale.
  zim::TitleIndex* titleIndex() { return index_ && index_->isOpen() ? index_.get() : nullptr; }
  const Collection* openCollection() const {
    return openIndex_ < collections_.size() ? &collections_[openIndex_] : nullptr;
  }
  void close();

  // Titles in the open collection starting with `query`, best first, at most
  // `max`, redirects to the same article collapsed. Uses the card's .pltitles
  // index (case- and accent-insensitive) when there is one, else the ZIM's
  // own byte-ordered title list (case-sensitive). Returns false if nothing
  // is open.
  struct Hit {
    uint32_t entry = 0;  // directory index (may be a redirect; the reader resolves it)
    std::string title;
  };
  bool search(std::string_view query, size_t max, std::vector<Hit>& out);

  OpenTimings lastOpen;
  uint32_t lastSearchMs = 0;

 private:
  Library() = default;
  bool loadManifest();
  void scanFolders();

  std::vector<Collection> collections_;
  std::string loadError_;
  std::unique_ptr<zim::Archive> archive_;
  std::unique_ptr<zim::TitleIndex> index_;
  std::unique_ptr<zim::TitleIndex::Cursor> cursor_;  // one 4 KB leaf page, reused per keystroke
  size_t openIndex_ = SIZE_MAX;
};

// "52.7 GB", "272 MB"
std::string formatBytes(uint64_t bytes);
// "19,191,219"
std::string formatCount(uint32_t n);

}  // namespace pocketlib
