// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#include "ZimTitleIndex.h"

#include <cstring>

#include "ZimFold.h"

namespace zim {
namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

constexpr uint8_t kLeaf = 1;
constexpr uint8_t kInner = 2;
constexpr size_t kPageHeader = 4;

}  // namespace

Error TitleIndex::open(std::unique_ptr<Source> source) {
  source_.reset();
  header_ = TitleIndexHeader();
  if (!source) return Error::Io;
  if (source->size() < kTitleIndexPageSize) return Error::BadMagic;
  uint8_t h[64];
  if (!source->read(0, h, sizeof h)) return Error::Io;
  if (std::memcmp(h, kTitleIndexMagic, 8) != 0) return Error::BadMagic;
  TitleIndexHeader hd;
  hd.version = rd32(h + 8);
  hd.pageSize = rd32(h + 12);
  hd.foldVersion = rd32(h + 16);
  hd.recordCount = rd32(h + 20);
  hd.firstLeaf = rd32(h + 24);
  hd.leafCount = rd32(h + 28);
  hd.root = rd32(h + 32);
  hd.levels = rd32(h + 36);
  std::memcpy(hd.zimUuid, h + 40, 16);
  hd.zimEntryCount = rd32(h + 56);
  if (hd.version < kTitleIndexMinVersion || hd.version > kTitleIndexVersion) return Error::BadVersion;
  if (hd.pageSize != kTitleIndexPageSize) return Error::BadHeader;
  if (hd.foldVersion != kFoldVersion) return Error::BadVersion;
  const uint64_t pages = source->size() / kTitleIndexPageSize;
  if (pages > 0xffffffffull || source->size() % kTitleIndexPageSize != 0) return Error::BadHeader;
  const uint32_t pageCount = static_cast<uint32_t>(pages);
  if (hd.leafCount == 0 || hd.firstLeaf == 0 || hd.firstLeaf >= pageCount || hd.leafCount > pageCount - hd.firstLeaf ||
      hd.root == 0 || hd.root >= pageCount || hd.levels > 16) {
    return Error::BadHeader;
  }
  header_ = hd;
  pageCount_ = pageCount;
  source_ = std::move(source);
  return Error::None;
}

bool TitleIndex::matches(const Archive& archive) const {
  return isOpen() && std::memcmp(header_.zimUuid, archive.header().uuid, 16) == 0 &&
         header_.zimEntryCount == archive.entryCount();
}

Error TitleIndex::readPage(uint32_t page, uint8_t* buf) {
  if (page == 0 || page >= pageCount_) return Error::BadCluster;
  if (!source_->read(static_cast<uint64_t>(page) * kTitleIndexPageSize, buf, kTitleIndexPageSize)) return Error::Io;
  return Error::None;
}

Error TitleIndex::loadLeaf(uint32_t page, Cursor& cursor) {
  if (page < header_.firstLeaf || page - header_.firstLeaf >= header_.leafCount) {
    cursor.page_ = 0;  // past the last leaf
    return Error::None;
  }
  const Error err = readPage(page, cursor.buf_);
  if (err != Error::None) return err;
  if (cursor.buf_[0] != kLeaf) return Error::BadCluster;
  cursor.page_ = page;
  cursor.remaining_ = rd16(cursor.buf_ + 2);
  cursor.offset_ = kPageHeader;
  cursor.key_.clear();
  return Error::None;
}

Error TitleIndex::seek(std::string_view foldedKey, Cursor& cursor) {
  if (!isOpen()) return Error::NotFound;
  cursor.page_ = 0;
  // Descend: in each inner page take the last child whose first key is
  // strictly below the target (equal keys may continue from the child before).
  uint32_t page = header_.root;
  uint8_t* buf = cursor.buf_;
  for (uint32_t level = header_.levels; level > 0; --level) {
    Error err = readPage(page, buf);
    if (err != Error::None) return err;
    if (buf[0] != kInner) return Error::BadCluster;
    const uint16_t count = rd16(buf + 2);
    if (count == 0) return Error::BadCluster;
    size_t off = kPageHeader;
    uint32_t chosen = 0;
    for (uint16_t i = 0; i < count; ++i) {
      if (off + 1 > kTitleIndexPageSize) return Error::BadCluster;
      const uint8_t len = buf[off];
      if (off + 1 + len + 4 > kTitleIndexPageSize) return Error::BadCluster;
      const std::string_view key(reinterpret_cast<const char*>(buf + off + 1), len);
      const uint32_t child = rd32(buf + off + 1 + len);
      if (i == 0 || key < foldedKey) {
        chosen = child;
      } else {
        break;
      }
      off += 1 + len + 4;
    }
    page = chosen;
  }
  Error err = loadLeaf(page, cursor);
  if (err != Error::None) return err;
  if (cursor.page_ == 0) return Error::BadCluster;
  // Skip records below the target; if the leaf runs out, the first match is
  // at the start of a later leaf.
  for (;;) {
    if (cursor.atEnd()) return Error::None;
    if (cursor.remaining_ == 0) {
      err = loadLeaf(cursor.page_ + 1, cursor);
      if (err != Error::None) return err;
      continue;
    }
    const size_t saveOffset = cursor.offset_;
    const uint16_t saveRemaining = cursor.remaining_;
    const std::string saveKey = cursor.key_;
    TitleRecord rec;
    err = next(cursor, rec);  // stays in this leaf: remaining_ was > 0
    if (err != Error::None) return err;
    if (rec.key >= foldedKey) {
      cursor.offset_ = saveOffset;
      cursor.remaining_ = saveRemaining;
      cursor.key_ = saveKey;
      return Error::None;
    }
  }
}

Error TitleIndex::next(Cursor& cursor, TitleRecord& out) {
  while (!cursor.atEnd() && cursor.remaining_ == 0) {
    const Error err = loadLeaf(cursor.page_ + 1, cursor);
    if (err != Error::None) return err;
  }
  if (cursor.atEnd()) return Error::NotFound;
  const uint8_t* b = cursor.buf_;
  size_t off = cursor.offset_;
  if (off + 2 > kTitleIndexPageSize) return Error::BadCluster;
  const uint8_t shared = b[off];
  const uint8_t suffix = b[off + 1];
  const size_t tail = header_.version >= 2 ? 5 : 4;  // entry (+ score)
  if (shared > cursor.key_.size() || off + 2 + suffix + tail > kTitleIndexPageSize) return Error::BadCluster;
  cursor.key_.resize(shared);
  cursor.key_.append(reinterpret_cast<const char*>(b + off + 2), suffix);
  out.key = cursor.key_;
  out.entry = rd32(b + off + 2 + suffix);
  out.score = tail == 5 ? b[off + 2 + suffix + 4] : 0;
  cursor.offset_ = off + 2 + suffix + tail;
  --cursor.remaining_;
  return Error::None;
}

}  // namespace zim
