// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace zim {

// Random-access byte source. The archive reads everything through this, so
// the same parser runs over a POSIX file on a Mac and a HalFile (or raw SD
// sectors) on the device. Implementations must be safe to call with any
// offset: reads past the end return false rather than crashing.
class Source {
 public:
  virtual ~Source() = default;
  virtual uint64_t size() const = 0;
  // Reads exactly `len` bytes at `offset`. Returns false on short read or
  // I/O error; the destination contents are then unspecified.
  virtual bool read(uint64_t offset, void* dst, size_t len) = 0;
};

// Several parts presented as one continuous byte range: name.zimaa,
// name.zimab, ... Reads that straddle a part boundary are split.
class SplitSource final : public Source {
 public:
  // Parts in order; empty parts are allowed but pointless.
  bool addPart(std::unique_ptr<Source> part);
  size_t partCount() const { return parts_.size(); }

  uint64_t size() const override { return total_; }
  bool read(uint64_t offset, void* dst, size_t len) override;

 private:
  struct Part {
    std::unique_ptr<Source> source;
    uint64_t start;  // offset of this part within the whole
    uint64_t size;
  };
  std::vector<Part> parts_;
  uint64_t total_ = 0;
};

// Large buffers (decompressed clusters, a few MB each) go through this so
// the device build can place them in PSRAM. Default: malloc/free.
struct Allocator {
  void* (*allocate)(size_t bytes);
  void (*release)(void* ptr);
  static const Allocator& defaultAllocator();
};

}  // namespace zim
