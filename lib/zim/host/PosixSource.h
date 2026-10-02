// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

// Host-only (macOS/Linux) file access for the ZIM reader: tests and zimcat.

#include <memory>
#include <string>
#include <vector>

#include "ZimSource.h"

namespace zim {

class PosixSource final : public Source {
 public:
  static std::unique_ptr<PosixSource> open(const std::string& path);
  ~PosixSource() override;
  uint64_t size() const override { return size_; }
  bool read(uint64_t offset, void* dst, size_t len) override;

 private:
  PosixSource(int fd, uint64_t size) : fd_(fd), size_(size) {}
  int fd_;
  uint64_t size_;
};

// Finds the parts of an archive given any of: "name.zim", "name.zimaa" or
// "name" (the shared prefix). A plain .zim wins if it exists; otherwise the
// .zimaa, .zimab, ... sequence is collected until the first gap.
std::vector<std::string> findParts(const std::string& path);

// Opens one file or a split set as a single Source. Null if nothing found.
std::unique_ptr<Source> openArchiveSource(const std::string& path, size_t* partCount = nullptr);

}  // namespace zim
