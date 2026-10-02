// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#include "TitleIndexWriter.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ZimFold.h"
#include "ZimTitleIndex.h"

namespace zim {
namespace {

constexpr size_t kPage = kTitleIndexPageSize;
constexpr size_t kPageHeader = 4;
constexpr uint8_t kLeaf = 1;
constexpr uint8_t kInner = 2;

void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

struct Child {
  std::string firstKey;
  uint32_t page;
};

class PageFile {
 public:
  explicit PageFile(std::FILE* f) : f_(f) {}
  // Writes `page` as page number `next()`; returns false on I/O error.
  bool append(const uint8_t* page) {
    if (std::fwrite(page, 1, kPage, f_) != kPage) return false;
    ++next_;
    return true;
  }
  uint32_t next() const { return next_; }

 private:
  std::FILE* f_;
  uint32_t next_ = 1;  // page 0 is the header, written last
};

}  // namespace

Error collectTitles(Archive& archive, TitleIndexWriter& writer, void (*progress)(uint32_t, uint32_t)) {
  const uint32_t total = archive.entryCount();
  std::vector<bool> wanted;
  const bool useList = archive.hasArticleList();
  if (useList) {
    wanted.assign(total, false);
    std::vector<uint32_t> chunk(1u << 18);
    for (uint32_t pos = 0; pos < archive.articleListCount();) {
      const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(chunk.size()), archive.articleListCount() - pos);
      const Error err = archive.articleListIndices(pos, n, chunk.data());
      if (err != Error::None) return err;
      for (uint32_t i = 0; i < n; ++i) wanted[chunk[i]] = true;
      pos += n;
    }
  }
  const char ns = archive.contentNamespace();
  Entry e;
  for (uint32_t i = 0; i < total; ++i) {
    if (progress && i % (1u << 20) == 0) progress(i, total);
    if (useList && !wanted[i]) continue;
    const Error err = archive.entryAt(i, e);
    if (err != Error::None) return err;
    if (!useList) {
      if (e.ns != ns) continue;
      if (!e.isRedirect() && archive.mimeType(e.mime).rfind("text/html", 0) != 0) continue;
    }
    writer.add(foldKey(e.title), i);
  }
  if (progress) progress(total, total);
  return Error::None;
}

void TitleIndexWriter::add(std::string_view key, uint32_t entry) {
  if (key.size() > kMaxKeyBytes) key = key.substr(0, kMaxKeyBytes);
  records_.push_back(Record{arena_.size(), entry, static_cast<uint8_t>(key.size())});
  arena_.insert(arena_.end(), key.begin(), key.end());
}

bool TitleIndexWriter::write(const std::string& path, const uint8_t zimUuid[16], uint32_t zimEntryCount,
                             std::string* error) {
  const auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  if (records_.size() > 0xffffffffull) return fail("too many titles");
  std::sort(records_.begin(), records_.end(), [this](const Record& a, const Record& b) {
    const int c = keyOf(a).compare(keyOf(b));
    return c != 0 ? c < 0 : a.entry < b.entry;
  });

  const std::string tmp = path + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return fail("cannot create " + tmp);
  uint8_t page[kPage];
  std::memset(page, 0, kPage);
  bool ok = std::fwrite(page, 1, kPage, f) == kPage;  // header placeholder
  PageFile out(f);

  // Leaves.
  std::vector<Child> level;
  size_t off = kPageHeader;
  uint16_t count = 0;
  std::string prev;
  const auto flushLeaf = [&]() {
    page[0] = kLeaf;
    put16(page + 2, count);
    ok = ok && out.append(page);
    std::memset(page, 0, kPage);
    off = kPageHeader;
    count = 0;
    prev.clear();
  };
  const uint32_t firstLeaf = out.next();
  for (const Record& r : records_) {
    const std::string_view key = keyOf(r);
    size_t shared = 0;
    if (count > 0) {
      const size_t limit = std::min(prev.size(), key.size());
      while (shared < limit && prev[shared] == key[shared]) ++shared;
    }
    if (off + 2 + (key.size() - shared) + 4 > kPage) {
      flushLeaf();
      shared = 0;
    }
    if (count == 0) level.push_back(Child{std::string(key), out.next()});
    page[off] = static_cast<uint8_t>(shared);
    page[off + 1] = static_cast<uint8_t>(key.size() - shared);
    std::memcpy(page + off + 2, key.data() + shared, key.size() - shared);
    put32(page + off + 2 + key.size() - shared, r.entry);
    off += 2 + (key.size() - shared) + 4;
    ++count;
    prev.assign(key);
  }
  if (count > 0 || level.empty()) {
    if (level.empty()) level.push_back(Child{std::string(), out.next()});
    flushLeaf();
  }
  const uint32_t leafCount = out.next() - firstLeaf;

  // Inner levels, bottom-up, until one page holds every child.
  uint32_t levels = 0;
  while (level.size() > 1) {
    std::vector<Child> upper;
    off = kPageHeader;
    count = 0;
    const auto flushInner = [&]() {
      page[0] = kInner;
      put16(page + 2, count);
      ok = ok && out.append(page);
      std::memset(page, 0, kPage);
      off = kPageHeader;
      count = 0;
    };
    for (const Child& c : level) {
      if (off + 1 + c.firstKey.size() + 4 > kPage) flushInner();
      if (count == 0) upper.push_back(Child{c.firstKey, out.next()});
      page[off] = static_cast<uint8_t>(c.firstKey.size());
      std::memcpy(page + off + 1, c.firstKey.data(), c.firstKey.size());
      put32(page + off + 1 + c.firstKey.size(), c.page);
      off += 1 + c.firstKey.size() + 4;
      ++count;
    }
    if (count > 0) flushInner();
    level.swap(upper);
    ++levels;
  }

  // Header.
  std::memset(page, 0, kPage);
  std::memcpy(page, kTitleIndexMagic, 8);
  put32(page + 8, kTitleIndexVersion);
  put32(page + 12, kTitleIndexPageSize);
  put32(page + 16, kFoldVersion);
  put32(page + 20, static_cast<uint32_t>(records_.size()));
  put32(page + 24, firstLeaf);
  put32(page + 28, leafCount);
  put32(page + 32, level.front().page);
  put32(page + 36, levels);
  std::memcpy(page + 40, zimUuid, 16);
  put32(page + 56, zimEntryCount);
  ok = ok && std::fseek(f, 0, SEEK_SET) == 0 && std::fwrite(page, 1, kPage, f) == kPage;
  ok = std::fclose(f) == 0 && ok;
  if (!ok) {
    std::remove(tmp.c_str());
    return fail("write failed for " + tmp);
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return fail("cannot rename " + tmp + " to " + path);
  }
  return true;
}

}  // namespace zim
