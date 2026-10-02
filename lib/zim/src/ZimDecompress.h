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

#include "ZimSource.h"

namespace zim {

enum class DecodeStatus : uint8_t { Ok, TooLarge, NoMemory, Corrupt };

// On success `data` is owned by the caller and must be freed with the same
// allocator. Each cluster is decoded in a single pass straight into its own
// buffer, so no separate decoder window is allocated (Kiwix's zstd frames
// declare an 8 MiB window that streaming decoders would otherwise reserve).
struct DecodeResult {
  DecodeStatus status = DecodeStatus::Corrupt;
  uint8_t* data = nullptr;
  size_t size = 0;
};

DecodeResult decodeZstd(const uint8_t* src, size_t srcSize, size_t maxOut, const Allocator& alloc);
DecodeResult decodeXz(const uint8_t* src, size_t srcSize, size_t maxOut, const Allocator& alloc);

}  // namespace zim
