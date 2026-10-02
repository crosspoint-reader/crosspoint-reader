// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#include "ZimDecompress.h"

#define ZSTD_STATIC_LINKING_ONLY
#include "third_party/zstd/zstd.h"

// Must match xz_config.h, which enables CRC64 for the decoder sources.
#define XZ_USE_CRC64
extern "C" {
#include "third_party/xz/xz.h"
}

#include <algorithm>

namespace zim {

namespace {

void* zstdAlloc(void* opaque, size_t size) { return static_cast<const Allocator*>(opaque)->allocate(size); }
void zstdFree(void* opaque, void* ptr) {
  if (ptr) static_cast<const Allocator*>(opaque)->release(ptr);
}

}  // namespace

DecodeResult decodeZstd(const uint8_t* src, size_t srcSize, size_t maxOut, const Allocator& alloc) {
  DecodeResult r;
  // Exact size when every frame records it (Kiwix's do); otherwise an upper
  // bound computed from the frame headers.
  unsigned long long outSize = ZSTD_findDecompressedSize(src, srcSize);
  if (outSize == ZSTD_CONTENTSIZE_ERROR) return r;
  if (outSize == ZSTD_CONTENTSIZE_UNKNOWN) {
    outSize = ZSTD_decompressBound(src, srcSize);
    if (outSize == ZSTD_CONTENTSIZE_ERROR) return r;
  }
  if (outSize > maxOut) {
    r.status = DecodeStatus::TooLarge;
    return r;
  }

  const ZSTD_customMem mem = {&zstdAlloc, &zstdFree, const_cast<Allocator*>(&alloc)};
  ZSTD_DCtx* dctx = ZSTD_createDCtx_advanced(mem);
  if (!dctx) {
    r.status = DecodeStatus::NoMemory;
    return r;
  }
  auto* out = static_cast<uint8_t*>(alloc.allocate(outSize ? static_cast<size_t>(outSize) : 1));
  if (!out) {
    ZSTD_freeDCtx(dctx);
    r.status = DecodeStatus::NoMemory;
    return r;
  }
  const size_t got = ZSTD_decompressDCtx(dctx, out, static_cast<size_t>(outSize), src, srcSize);
  ZSTD_freeDCtx(dctx);
  if (ZSTD_isError(got)) {
    alloc.release(out);
    return r;
  }
  r.status = DecodeStatus::Ok;
  r.data = out;
  r.size = got;
  return r;
}

DecodeResult decodeXz(const uint8_t* src, size_t srcSize, size_t maxOut, const Allocator& alloc) {
  DecodeResult r;
  static bool crcReady = false;
  if (!crcReady) {
    xz_crc32_init();
    xz_crc64_init();
    crcReady = true;
  }

  // xz records no total size up front; single-call mode needs the whole
  // output buffer, so start generous and double on "buffer too small".
  size_t cap = std::min(maxOut, std::max<size_t>(1u << 20, srcSize * 8));
  while (true) {
    auto* out = static_cast<uint8_t*>(alloc.allocate(cap));
    if (!out) {
      r.status = DecodeStatus::NoMemory;
      return r;
    }
    xz_dec* dec = xz_dec_init(XZ_SINGLE, 0);
    if (!dec) {
      alloc.release(out);
      r.status = DecodeStatus::NoMemory;
      return r;
    }
    xz_buf b{};
    b.in = src;
    b.in_size = srcSize;
    b.out = out;
    b.out_size = cap;
    const xz_ret ret = xz_dec_run(dec, &b);
    xz_dec_end(dec);
    if (ret == XZ_STREAM_END) {
      r.status = DecodeStatus::Ok;
      r.data = out;
      r.size = b.out_pos;
      return r;
    }
    alloc.release(out);
    if (ret == XZ_MEM_ERROR) {
      r.status = DecodeStatus::NoMemory;
      return r;
    }
    if (ret != XZ_BUF_ERROR || b.out_pos < cap) return r;  // corrupt
    if (cap >= maxOut) {
      r.status = DecodeStatus::TooLarge;
      return r;
    }
    cap = std::min(maxOut, cap * 2);
  }
}

}  // namespace zim
