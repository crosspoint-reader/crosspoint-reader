// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#include "ZimSource.h"

#include <cstdlib>
#include <cstring>

namespace zim {

bool SplitSource::addPart(std::unique_ptr<Source> part) {
  if (!part) return false;
  const uint64_t partSize = part->size();
  if (total_ + partSize < total_) return false;  // overflow
  parts_.push_back(Part{std::move(part), total_, partSize});
  total_ += partSize;
  return true;
}

bool SplitSource::read(uint64_t offset, void* dst, size_t len) {
  if (len == 0) return true;
  if (offset > total_ || len > total_ - offset) return false;

  // Binary search for the part holding `offset`.
  size_t lo = 0;
  size_t hi = parts_.size();
  while (hi - lo > 1) {
    const size_t mid = lo + (hi - lo) / 2;
    if (parts_[mid].start <= offset) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  auto* out = static_cast<uint8_t*>(dst);
  size_t i = lo;
  while (len > 0) {
    if (i >= parts_.size()) return false;
    Part& p = parts_[i];
    const uint64_t within = offset - p.start;
    if (within >= p.size) {  // empty part or boundary: move on
      ++i;
      continue;
    }
    const uint64_t available = p.size - within;
    const size_t chunk = available < len ? static_cast<size_t>(available) : len;
    if (!p.source->read(within, out, chunk)) return false;
    out += chunk;
    offset += chunk;
    len -= chunk;
    ++i;
  }
  return true;
}

namespace {
void* mallocAllocate(size_t bytes) { return std::malloc(bytes); }
void mallocRelease(void* ptr) { std::free(ptr); }
}  // namespace

const Allocator& Allocator::defaultAllocator() {
  static const Allocator kMalloc{&mallocAllocate, &mallocRelease};
  return kMalloc;
}

}  // namespace zim
