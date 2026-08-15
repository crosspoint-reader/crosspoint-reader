#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <stdint.h>

#include <cstdlib>
#include <cstring>
#include <string>

// The .pxc layout this writer produces, plus the hook that hands a whole-image
// buffer to the render cache on success.
#include "PxcFormat.h"

#ifdef CROSSPOINT_PSRAM_IMAGE_CACHE
#include <esp_heap_caps.h>
#endif

// Streaming cache writer for 2-bit pixels (4 levels). Packs 4 pixels per byte,
// MSB first.
//
// The .pxc file is written incrementally in small row bands rather than holding
// the whole decoded image in one heap buffer. A full-page image (e.g. 482x728)
// needs ~88KB packed, which will not fit alongside the ~20KB JPEG decoder on a
// fragmented 380KB heap (free heap is routinely ~55KB on an image page). When
// the cache cannot be written, every render pass re-decodes the JPEG from
// scratch; an anti-aliased image page renders ~14 times (BW + AA restore + two
// grayscale planes x ~6 strips), so a 2s decode becomes a ~30s freeze / watchdog
// reset. Streaming keeps the working set to a single MCU-row band, so caching
// succeeds and the image is decoded exactly once.
//
// Correctness relies on JPEGDEC delivering blocks in raster MCU order (outer
// loop over y, inner over x: see jpeg.inl DecodeJPEG). Consecutive MCU rows map
// to contiguous, non-overlapping destination row ranges, so once a block whose
// top row is Y arrives, every output row < Y is final and is flushed to disk.
struct PixelCache {
  uint8_t* buffer;   // band buffer: (bandRows + 1) rows; last row kept zeroed
  uint8_t* zeroRow;  // points at the spare zeroed row, for gap/clip fill
  int width;
  int height;
  int bytesPerRow;
  int originX;      // config.x - to convert screen coords to cache coords
  int originY;      // config.y
  int bandRows;     // rows held in the band buffer
  int bandStart;    // image-local row index of band buffer row 0
  int flushedRows;  // image-local rows already written to file
  HalFile file;
  std::string cachePathStr;
  bool ok;

#ifdef CROSSPOINT_PSRAM_IMAGE_CACHE
  // --- One-shot mode -----------------------------------------------------------
  // With PSRAM available the band can simply BE the whole image: `whole` is one
  // heap_caps_malloc(MALLOC_CAP_SPIRAM) block holding a complete .pxc (4-byte
  // header + payload), `buffer` points at its payload, and the band never
  // moves. advanceTo() then has nothing to flush and finalize() writes the file
  // in a SINGLE write instead of the ~40 interleaved 3 KB band flushes a
  // full-page image costs today -- and hands the block straight to ImageBlock's
  // render cache, so the ~12 render passes that follow never read it back.
  //
  // Everything about this is optional: an image past the cache's payload cap,
  // or a failed PSRAM allocation, leaves `whole` null and the banded path below
  // runs exactly as before.
  uint8_t* whole = nullptr;
#endif

  PixelCache()
      : buffer(nullptr),
        zeroRow(nullptr),
        width(0),
        height(0),
        bytesPerRow(0),
        originX(0),
        originY(0),
        bandRows(0),
        bandStart(0),
        flushedRows(0),
        ok(false) {}
  PixelCache(const PixelCache&) = delete;
  PixelCache& operator=(const PixelCache&) = delete;

  static constexpr int MIN_BAND_ROWS = 16;
  static constexpr size_t MAX_BAND_BYTES = 24 * 1024;  // band working-set ceiling

  // Open the cache file, write the header, and allocate a band buffer big enough
  // to hold the tallest single decode block (maxBlockDstRows output rows).
  bool begin(const std::string& cachePath, int w, int h, int ox, int oy, int maxBlockDstRows) {
    width = w;
    height = h;
    originX = ox;
    originY = oy;
    bytesPerRow = (w + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
    bandStart = 0;
    flushedRows = 0;
    ok = false;

#ifdef CROSSPOINT_PSRAM_IMAGE_CACHE
    if (!beginWholeBuffer(w, h) && !beginBandBuffer(w, h, maxBlockDstRows)) return false;
#else
    if (!beginBandBuffer(w, h, maxBlockDstRows)) return false;
#endif

    if (!Storage.openFileForWrite("IMG", cachePath, file)) {
      LOG_ERR("IMG", "Failed to open cache file for writing: %s", cachePath.c_str());
      freeBuffers();
      return false;
    }
    cachePathStr = cachePath;

    // One-shot mode already carries the header at the head of `whole` and emits
    // it with the payload in finalize()'s single write; the banded path writes
    // it to the file now.
    bool headerInBuffer = false;
#ifdef CROSSPOINT_PSRAM_IMAGE_CACHE
    headerInBuffer = whole != nullptr;
#endif
    if (!headerInBuffer) {
      uint16_t w16 = (uint16_t)w;
      uint16_t h16 = (uint16_t)h;
      if (file.write(&w16, 2) != 2 || file.write(&h16, 2) != 2) {
        LOG_ERR("IMG", "Failed to write cache header: %s", cachePath.c_str());
        abort();        // closes the file and removes the stub
        freeBuffers();  // ...and the band buffer this call allocated above
        return false;
      }
    }

    LOG_DBG("IMG", "Cache stream started: %s (%dx%d, band %d rows)", cachePath.c_str(), w, h, bandRows);
    ok = true;
    return true;
  }

  // Band buffer for the streaming path: enough rows for the tallest single
  // decode block (maxBlockDstRows output rows), plus a spare zeroed row.
  // Takes (w, h) like beginWholeBuffer so its diagnostics describe the image it
  // was asked for rather than whatever the members happen to hold.
  bool beginBandBuffer(int w, int h, int maxBlockDstRows) {
    int wantRows = maxBlockDstRows + 2;
    if (wantRows < MIN_BAND_ROWS) wantRows = MIN_BAND_ROWS;
    if (wantRows > h) wantRows = h;

    size_t maxRowsByMem = MAX_BAND_BYTES / (size_t)bytesPerRow;
    if (maxRowsByMem < 1) maxRowsByMem = 1;
    if ((size_t)wantRows > maxRowsByMem) wantRows = (int)maxRowsByMem;

    // A single decode block must fit inside the band, otherwise streaming would
    // drop rows. This only fails for pathological upscales that could not be
    // cached at all; fall back to the no-cache path.
    if (wantRows < maxBlockDstRows) {
      LOG_ERR("IMG", "Cache band too small (%d < %d rows) for %dx%d", wantRows, maxBlockDstRows, w, h);
      return false;
    }
    bandRows = wantRows;

    const size_t bufSize = (size_t)(bandRows + 1) * bytesPerRow;  // +1 spare zero row
    buffer = (uint8_t*)malloc(bufSize);
    if (!buffer) {
      LOG_ERR("IMG", "OOM cache band: %u bytes", (unsigned)bufSize);
      return false;
    }
    memset(buffer, 0, bufSize);
    zeroRow = buffer + (size_t)bandRows * bytesPerRow;
    return true;
  }

#ifdef CROSSPOINT_PSRAM_IMAGE_CACHE
  // One-shot mode: one PSRAM block holding the complete .pxc. False (with
  // `whole` left null) whenever the image is past the cache's payload cap or
  // PSRAM cannot provide the block, which falls back to the banded path.
  bool beginWholeBuffer(int w, int h) {
    const size_t payload = (size_t)bytesPerRow * h;
    if (payload == 0 || payload > PXC_MAX_PAYLOAD_BYTES) return false;
    const size_t total = PXC_HEADER_BYTES + payload;

    whole = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!whole) {
      LOG_DBG("IMG", "No PSRAM for a %u-byte image buffer; streaming the cache", (unsigned)total);
      return false;
    }
    // Zeroed up front so rows the decode never covers (a clipped image) stay
    // black, exactly as the banded path's zero-fill leaves them.
    memset(whole, 0, total);
    const uint16_t w16 = (uint16_t)w;
    const uint16_t h16 = (uint16_t)h;
    memcpy(whole, &w16, 2);
    memcpy(whole + 2, &h16, 2);

    buffer = whole + PXC_HEADER_BYTES;
    zeroRow = nullptr;  // the band never moves, so no gap/clip fill is needed
    bandRows = h;       // the band IS the image: every row is always in range
    return true;
  }
#endif

  // Flush every output row below newTopRow (they are final in raster order) and
  // reposition the band to start at newTopRow. Returns false if a write failed,
  // in which case the caller must stop caching for the rest of the decode.
  bool advanceTo(int newTopRow) {
    if (!ok) return false;
#ifdef CROSSPOINT_PSRAM_IMAGE_CACHE
    // One-shot mode: every row is already in its final place in `whole` and the
    // band never moves, so there is nothing to flush or reposition.
    if (whole) return true;
#endif
    if (newTopRow <= bandStart) return true;
    if (newTopRow > height) newTopRow = height;

    for (int r = bandStart; r < newTopRow; ++r) {
      const int idx = r - bandStart;
      const uint8_t* rowPtr = (idx < bandRows) ? (buffer + (size_t)idx * bytesPerRow) : zeroRow;
      if (file.write(rowPtr, (size_t)bytesPerRow) != (size_t)bytesPerRow) {
        LOG_ERR("IMG", "Cache write error at row %d", r);
        ok = false;
        return false;
      }
    }
    flushedRows = newTopRow;
    bandStart = newTopRow;
    memset(buffer, 0, (size_t)bandRows * bytesPerRow);  // fresh band (gaps stay black)
    return true;
  }

  // Flush the final band and zero-fill any rows never covered (image clipped by
  // the screen), then close the file.
  bool finalize() {
    if (!ok) {
      abort();
      return false;
    }
#ifdef CROSSPOINT_PSRAM_IMAGE_CACHE
    if (whole) return finalizeWhole();
#endif
    for (int r = flushedRows; r < height; ++r) {
      const int idx = r - bandStart;
      const uint8_t* rowPtr = (idx >= 0 && idx < bandRows) ? (buffer + (size_t)idx * bytesPerRow) : zeroRow;
      if (file.write(rowPtr, (size_t)bytesPerRow) != (size_t)bytesPerRow) {
        LOG_ERR("IMG", "Cache write error at row %d", r);
        abort();
        return false;
      }
    }
    file.close();
    LOG_DBG("IMG", "Cache written: %s (%dx%d, %d bytes)", cachePathStr.c_str(), width, height,
            4 + bytesPerRow * height);
    ok = false;  // file handed off; nothing left to clean up
    return true;
  }

#ifdef CROSSPOINT_PSRAM_IMAGE_CACHE
  // One-shot finish: header + payload in a SINGLE write, then hand the block to
  // the render cache instead of making the passes that follow read it back.
  //
  // Donating is only legal from a decode that holds the RenderLock, because
  // that cache has no lock of its own. It holds here by construction: a
  // PixelCache exists only for a decode with config.cachePath set, and the sole
  // producer of those is ImageBlock::render(), which the render task runs under
  // the lock. Any future off-lock decode that wants a .pxc has to opt out of the
  // donation before it can use this writer.
  bool finalizeWhole() {
    const size_t total = PXC_HEADER_BYTES + (size_t)bytesPerRow * height;
    if (file.write(whole, total) != total) {
      LOG_ERR("IMG", "Cache write error: %s", cachePathStr.c_str());
      abort();
      return false;
    }
    file.close();
    ok = false;  // file handed off; nothing left to clean up
    LOG_DBG("IMG", "Cache written in one pass: %s (%dx%d, %u bytes)", cachePathStr.c_str(), width, height,
            (unsigned)total);

    if (adoptPxcImage(cachePathStr, whole, total)) {
      whole = nullptr;  // ownership transferred to the render cache
      buffer = nullptr;
    }
    return true;
  }
#endif

  // Drop a partial/failed cache so a later decode re-creates it cleanly.
  void abort() {
    if (file.isOpen()) file.close();
    if (!cachePathStr.empty()) {
      Storage.remove(cachePathStr.c_str());
    }
    ok = false;
  }

  // Release whichever buffer this cache owns. Safe to call repeatedly.
  void freeBuffers() {
#ifdef CROSSPOINT_PSRAM_IMAGE_CACHE
    if (whole) {
      free(whole);  // heap_caps_malloc'd memory is free()-compatible
      whole = nullptr;
      buffer = nullptr;
      zeroRow = nullptr;
      return;
    }
#endif
    if (buffer) {
      free(buffer);
      buffer = nullptr;
      zeroRow = nullptr;
    }
  }

  ~PixelCache() {
    if (file.isOpen()) {
      // The file is still open, so neither finalize() nor abort() ran, or a
      // mid-stream write failed (advanceTo() cleared ok but left the file open).
      // Drop the partial cache so we leave no corrupt file behind.
      abort();
    }
    freeBuffers();
  }
};
