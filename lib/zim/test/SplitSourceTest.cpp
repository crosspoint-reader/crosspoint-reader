// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>

#include "ZimSource.h"

namespace {

class MemorySource final : public zim::Source {
 public:
  explicit MemorySource(std::string data) : data_(std::move(data)) {}
  uint64_t size() const override { return data_.size(); }
  bool read(uint64_t offset, void* dst, size_t len) override {
    if (offset > data_.size() || len > data_.size() - offset) return false;
    std::memcpy(dst, data_.data() + offset, len);
    return true;
  }

 private:
  std::string data_;
};

zim::SplitSource makeSplit(const std::string& whole, const std::initializer_list<size_t>& cuts) {
  zim::SplitSource split;
  size_t prev = 0;
  for (size_t cut : cuts) {
    split.addPart(std::make_unique<MemorySource>(whole.substr(prev, cut - prev)));
    prev = cut;
  }
  split.addPart(std::make_unique<MemorySource>(whole.substr(prev)));
  return split;
}

}  // namespace

TEST(SplitSource, EveryRangeMatchesTheWhole) {
  std::string whole;
  for (int i = 0; i < 1000; ++i) whole.push_back(static_cast<char>('a' + i % 26));
  // Includes an empty part (cut at 500 twice) to exercise skipping it.
  zim::SplitSource split = makeSplit(whole, {7, 300, 500, 500, 999});
  ASSERT_EQ(split.size(), whole.size());
  ASSERT_EQ(split.partCount(), 6u);

  std::string buf;
  for (size_t off = 0; off <= whole.size(); off += 13) {
    for (size_t len : {0u, 1u, 6u, 7u, 8u, 200u, 600u}) {
      buf.assign(len, '\0');
      const bool ok = split.read(off, buf.data(), len);
      if (off + len > whole.size()) {
        EXPECT_FALSE(ok) << off << "+" << len;
      } else {
        ASSERT_TRUE(ok) << off << "+" << len;
        EXPECT_EQ(buf, whole.substr(off, len)) << off << "+" << len;
      }
    }
  }
}

TEST(SplitSource, RejectsReadsPastTheEnd) {
  zim::SplitSource split = makeSplit("hello world", {5});
  char c;
  EXPECT_FALSE(split.read(11, &c, 1));
  EXPECT_FALSE(split.read(~0ull, &c, 1));
  EXPECT_TRUE(split.read(10, &c, 1));
  EXPECT_EQ(c, 'd');
}
