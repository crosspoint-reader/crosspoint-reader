// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

// Reader for a Pocket Library title index: a sidecar file the card builder
// writes next to each ZIM so the device can search titles case- and
// accent-insensitively. It is a static B-tree of 4 KB pages keyed by
// foldKey(title); finding the first match for a prefix costs one read per
// level (two or three for English Wikipedia), then matches run on in order
// through consecutive leaf pages.
//
// File layout, all integers little-endian:
//   page 0      header
//     0  char[8] "PLTITLE\0"
//     8  u32     format version (1)
//    12  u32     page size (4096)
//    16  u32     fold version (kFoldVersion when built)
//    20  u32     record count
//    24  u32     first leaf page
//    28  u32     leaf page count (leaves are consecutive)
//    32  u32     root page
//    36  u32     levels above the leaves (0: the root is the only leaf)
//    40  u8[16]  UUID of the ZIM it indexes
//    56  u32     entry count of that ZIM
//   leaf page   u8 type (1), u8 0, u16 count, then per record:
//               u8 shared prefix with the previous key in this page,
//               u8 suffix length, suffix bytes, u32 ZIM entry index
//   inner page  u8 type (2), u8 0, u16 count, then per child:
//               u8 key length, key bytes (the child's first key), u32 page
// Records are sorted by (key, entry index).

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "ZimArchive.h"
#include "ZimSource.h"

namespace zim {

// 2 adds a popularity byte to every record (see collectTitles); 3 adds a
// flags byte and "word" records (a title from a later word on). 1 and 2 are
// still read: scores 0 and no word records respectively.
constexpr uint32_t kTitleIndexVersion = 3;
constexpr uint32_t kTitleIndexMinVersion = 1;
constexpr uint32_t kTitleIndexPageSize = 4096;
constexpr char kTitleIndexMagic[8] = {'P', 'L', 'T', 'I', 'T', 'L', 'E', '\0'};

struct TitleIndexHeader {
  uint32_t version = 0;
  uint32_t pageSize = 0;
  uint32_t foldVersion = 0;
  uint32_t recordCount = 0;
  uint32_t firstLeaf = 0;
  uint32_t leafCount = 0;
  uint32_t root = 0;
  uint32_t levels = 0;
  uint8_t zimUuid[16] = {};
  uint32_t zimEntryCount = 0;
};

struct TitleRecord {
  std::string key;  // folded title
  uint32_t entry = 0;
  uint8_t score = 0;  // popularity, 0..255 (version 2); 0 in version 1 files
  // Version 3: the record is the title from one of its later words on
  // ("panda" for "Giant panda"), so a search finds words inside titles.
  bool word = false;
};

class TitleIndex {
 public:
  // Position in the index; advance with next(). Holds one leaf page.
  class Cursor {
   public:
    bool atEnd() const { return page_ == 0; }

   private:
    friend class TitleIndex;
    uint32_t page_ = 0;  // 0 = past the end
    uint16_t remaining_ = 0;
    size_t offset_ = 0;
    std::string key_;
    uint8_t buf_[kTitleIndexPageSize];
  };

  Error open(std::unique_ptr<Source> source);
  bool isOpen() const { return source_ != nullptr; }
  const TitleIndexHeader& header() const { return header_; }
  uint32_t recordCount() const { return header_.recordCount; }
  // True when this index was built for `archive` (same UUID and entry count).
  bool matches(const Archive& archive) const;

  // Places `cursor` on the first record whose key is >= foldedKey.
  Error seek(std::string_view foldedKey, Cursor& cursor);
  // Reads the record under the cursor and advances. NotFound at the end.
  Error next(Cursor& cursor, TitleRecord& out);

 private:
  Error readPage(uint32_t page, uint8_t* buf);
  Error loadLeaf(uint32_t page, Cursor& cursor);

  std::unique_ptr<Source> source_;
  TitleIndexHeader header_;
  uint32_t pageCount_ = 0;
};

// True if `key` starts with `prefix` (both already folded).
inline bool keyHasPrefix(std::string_view key, std::string_view prefix) {
  return key.size() >= prefix.size() && key.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace zim
