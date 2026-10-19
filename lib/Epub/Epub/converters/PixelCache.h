#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <stdint.h>

#include <cstring>
#include <string>

namespace image_scratch {
void noteHeapShort();  // ImageBlock.h: a cache band the heap refused is a decode short of memory
}  // namespace image_scratch

// Streaming cache writer for 2-bit pixels (4 levels) during decode.
// Packs 4 pixels per byte, MSB first.
//
// The .pxc file is written incrementally in small row bands rather than holding
// the whole decoded image in one heap buffer. A full-page image (e.g. 482x728)
// needs ~88KB packed, which will not fit alongside the ~20KB JPEG decoder on a
// fragmented ~380KB heap (free heap is routinely ~55KB on an image page). When
// the cache cannot be written, every render pass re-decodes the source image
// from scratch; an anti-aliased image page renders ~14 times (BW + AA restore +
// grayscale strip planes), so a multi-second decode becomes a UI freeze / crash.
// Streaming keeps the working set to a single MCU-row band, so caching succeeds
// and the image is decoded exactly once.
//
// Correctness relies on the decoder delivering blocks in raster top-to-bottom
// order with bounded per-block height (TJpgDec: raster MCU rows; PNGdec: one
// scanline at a time). Once a block whose top destination row is Y arrives,
// every output row < Y is final and can be flushed to disk; advanceTo() is told
// the tallest possible block height up front (maxBlockDstRows) so the band is
// always large enough to hold an in-flight block without losing rows.
//
// Cherry-picked and adapted from upstream commit d9bcef7a58f3e024129bfc55eb82c6be5d62a148
// ("fix: Replace full-image cache buffer with streaming band buffer to reduce
// memory usage", crosspoint-reader/crosspoint-reader#2230) — reworked against
// this fork's HalFile/FsFile storage layer and DirectCacheWriter interface.
struct PixelCache {
  uint8_t* buffer;   // band buffer: (bandRows + 1) rows; last row kept at FILL_BYTE
  uint8_t* fillRow;  // points at the spare pre-filled row, for gap/clip fill
  int width;
  int height;
  int bytesPerRow;
  int originX;       // config.x - to convert screen coords to cache coords
  int originY;       // config.y + bandStart - band-local screen-to-cache mapping
  int bandRows;      // rows held in the band buffer
  int bandStart;     // image-local row index of band buffer row 0
  int flushedRows;   // image-local rows already written to file
  int maxBlockRows;  // tallest single decode block, from begin() -- see advanceTo()
  FsFile file;
  std::string cachePathStr;  // the finished cache
  std::string partPathStr;   // where the rows go until finalize() renames it to cachePathStr
  bool ok;

  // Byte the band is (re)filled with: four pixels of value 3. 3 is the only value DirectPixelWriter
  // treats as "leave alone" in every render mode, so any pixel the decode never covers stays page
  // white. Filling with 0 instead made every uncovered pixel BLACK — and because those pixels are
  // written to the .pxc, one short decode (a box whose aspect ratio the decoder cannot fill, a
  // row lost to integer rounding, an image clipped by the screen) was replayed as a black band
  // under the picture on every later view of the page.
  static constexpr uint8_t FILL_BYTE = 0xFF;

  PixelCache()
      : buffer(nullptr),
        fillRow(nullptr),
        width(0),
        height(0),
        bytesPerRow(0),
        originX(0),
        originY(0),
        bandRows(0),
        bandStart(0),
        flushedRows(0),
        maxBlockRows(1),
        ok(false) {}
  PixelCache(const PixelCache&) = delete;
  PixelCache& operator=(const PixelCache&) = delete;

  static constexpr int MIN_BAND_ROWS = 16;
  static constexpr size_t MAX_BAND_BYTES = 24 * 1024;  // band working-set ceiling

  // .pxc format stamp, first uint16 of the file. The high bit distinguishes it from
  // the legacy unversioned header (which began with the width, capped at 0x7FFF by
  // validateImageDimensions), so readers can detect and delete pre-versioning files.
  // The low bits are the format version: bump when the *pixel content* semantics
  // change (e.g. the MCU-order dither fix), not just on code refactors — cached
  // files persist on SD across firmware updates and are replayed without re-decode.
  // v3: uncovered pixels are white (FILL_BYTE) instead of black — see FILL_BYTE. Existing
  //     caches carry the black band baked in, so they have to be re-decoded.
  // v4: JPEG downscaling area-averages instead of sampling one pixel, so thin lines
  //     survive. A v3 cache replays the nearest-neighbour picture forever otherwise.
  // v5: progressive JPEGs are fully decoded instead of shown from their 1/8-resolution DC scan.
  static constexpr uint16_t PXC_MAGIC = 0x8005;
  // Same layout, written by a decode that had to settle for less than the requested scale (a
  // progressive JPEG at a coarser DCT scale, or its DC-only preview) because the heap could not
  // hold the workspace. Readers replay it like any cache; a warm pass with the framebuffers
  // released, or the reader's memory ladder with memory freed, decodes it again and replaces it
  // when that decode finishes (ImageBlock::CoarseRedecodeScope). Before this the .pxc
  // had no quality key, so a preview-grade image stood in for the book's for good (memory audit
  // 2026-09, F3).
  static constexpr uint16_t PXC_MAGIC_COARSE = 0x8006;
  static bool magicIsValid(const uint16_t magic) { return magic == PXC_MAGIC || magic == PXC_MAGIC_COARSE; }
  bool coarse = false;
  // Flag the cache being written as a below-requested-quality decode; finalize() stamps it.
  void markCoarse() { coarse = true; }
  static constexpr size_t PXC_HEADER_BYTES = 6;  // magic + width + height

  // Rows begin() gives the band for a w x h image whose tallest decode block is maxBlockDstRows.
  static int bandRowsFor(const int w, const int h, const int maxBlockDstRows) {
    const size_t rowBytes = static_cast<size_t>((w + 3) / 4);  // 2 bits per pixel, 4 pixels per byte
    if (rowBytes == 0) return 0;

    int wantRows = maxBlockDstRows + 2;
    if (wantRows < MIN_BAND_ROWS) wantRows = MIN_BAND_ROWS;
    if (wantRows > h) wantRows = h;

    size_t maxRowsByMem = MAX_BAND_BYTES / rowBytes;
    if (maxRowsByMem < 1) maxRowsByMem = 1;
    if ((size_t)wantRows > maxRowsByMem) wantRows = (int)maxRowsByMem;
    return wantRows;
  }

  // Heap begin() takes for the same image: the band plus its spare fill row. This, not
  // MAX_BAND_BYTES, is what a caller's "can I afford to cache?" gate should charge.
  static size_t bandBytesFor(const int w, const int h, const int maxBlockDstRows) {
    const int rows = bandRowsFor(w, h, maxBlockDstRows);
    return rows > 0 ? static_cast<size_t>(rows + 1) * static_cast<size_t>((w + 3) / 4) : 0;
  }

  // The partial file of a cache being written (or parked): renamed to the cache path only once
  // every row is in it, so a reader never replays a half-written picture, and a checkpointed
  // decode (see park()/resume()) can leave it on the card and append to it later.
  static std::string partPathFor(const std::string& cachePath) { return cachePath + ".part"; }

  // Set up geometry and the band buffer for a w x h cache whose tallest decode block is
  // maxBlockDstRows output rows. Shared by begin() and resume().
  bool setUp(int w, int h, int ox, int oy, int maxBlockDstRows) {
    dropBuffer();  // a retry after a refused resume() sets up the same cache again
    coarse = false;
    width = w;
    height = h;
    originX = ox;
    originY = oy;
    bandStart = 0;
    flushedRows = 0;
    ok = false;

    bytesPerRow = (w + 3) / 4;  // 2 bits per pixel, 4 pixels per byte

    const int wantRows = bandRowsFor(w, h, maxBlockDstRows);

    // A single decode block must fit inside the band, otherwise streaming would
    // drop rows. This only fails for pathological upscales that could not be
    // cached at all; fall back to the no-cache path.
    if (wantRows < maxBlockDstRows) {
      LOG_ERR("IMG", "Cache band too small (%d < %d rows) for %dx%d", wantRows, maxBlockDstRows, w, h);
      return false;
    }
    bandRows = wantRows;
    maxBlockRows = maxBlockDstRows > 0 ? maxBlockDstRows : 1;

    const size_t bufSize = (size_t)(bandRows + 1) * bytesPerRow;  // +1 spare zero row
    buffer = static_cast<uint8_t*>(malloc(bufSize));
    if (!buffer) {
      LOG_ERR("IMG", "OOM cache band: %u bytes", (unsigned)bufSize);
      image_scratch::noteHeapShort();
      return false;
    }
    memset(buffer, FILL_BYTE, bufSize);
    fillRow = buffer + (size_t)bandRows * bytesPerRow;
    return true;
  }

  void dropBuffer() {
    free(buffer);
    buffer = nullptr;
    fillRow = nullptr;
  }

  // Open the cache file, write the header, and allocate a band buffer big enough
  // to hold the tallest single decode block (maxBlockDstRows output rows).
  bool begin(const std::string& cachePath, int w, int h, int ox, int oy, int maxBlockDstRows) {
    if (!setUp(w, h, ox, oy, maxBlockDstRows)) return false;
    const std::string partPath = partPathFor(cachePath);
    if (!Storage.openFileForWrite("IMG", partPath, file)) {
      LOG_ERR("IMG", "Failed to open cache file for writing: %s", partPath.c_str());
      dropBuffer();
      return false;
    }
    cachePathStr = cachePath;
    partPathStr = partPath;

    const uint16_t magic = PXC_MAGIC;
    uint16_t w16 = (uint16_t)w;
    uint16_t h16 = (uint16_t)h;
    if (file.write(reinterpret_cast<const uint8_t*>(&magic), 2) != 2 ||
        file.write(reinterpret_cast<const uint8_t*>(&w16), 2) != 2 ||
        file.write(reinterpret_cast<const uint8_t*>(&h16), 2) != 2) {
      LOG_ERR("IMG", "Failed to write cache header: %s", cachePath.c_str());
      abort();
      return false;
    }

    LOG_TRC("IMG", "Cache stream started: %s (%dx%d, band %d rows)", cachePath.c_str(), w, h, bandRows);
    ok = true;
    return true;
  }

  // Write rows [bandStart, newTopRow) and rebase the band. The rows still held in the band are
  // contiguous in `buffer`, so they go out as ONE write; only rows past the band's end (gaps
  // left by a clipped or short decode) fall back to the pre-filled spare row.
  bool flushThrough(int newTopRow) {
    const int pending = newTopRow - flushedRows;
    if (pending <= 0) return true;
    const int inBand = pending < bandRows ? pending : bandRows;

    const size_t runBytes = (size_t)inBand * bytesPerRow;
    if (inBand > 0 && file.write(buffer, runBytes) != runBytes) {
      LOG_ERR("IMG", "Cache write error at row %d", flushedRows);
      ok = false;
      return false;
    }
    for (int r = flushedRows + inBand; r < newTopRow; ++r) {
      if (file.write(fillRow, (size_t)bytesPerRow) != (size_t)bytesPerRow) {
        LOG_ERR("IMG", "Cache write error at row %d", r);
        ok = false;
        return false;
      }
    }
    flushedRows = newTopRow;
    bandStart = newTopRow;
    memset(buffer, FILL_BYTE, (size_t)bandRows * bytesPerRow);  // fresh band (gaps stay white)
    return true;
  }

  // Tell the cache that every output row below newTopRow is final. Returns false only if a
  // write failed, in which case the caller must stop caching for the rest of the decode.
  //
  // Final does NOT mean written yet. The PNG decoder calls this once per destination row, and
  // flushing on the spot meant one file.write() per row: a 464x618 cache went to disk as 618
  // separate 116-byte writes, plus a full band memset each time. Device-measured on X4, that
  // was ~2.3 s per cache -- roughly 40% of a decode, and the reason adding a second cache to
  // the same pass cost 2274 ms when its dither and packing are nearly free. (Same shape as the
  // 512-byte extract writes fixed in PR #220; small writes are simply very expensive here.)
  //
  // So rows accumulate in the band and go out in one call when the next block would no longer
  // fit. Deferring is always safe -- final rows are immutable, the band is already sized to
  // hold them, and finalize() writes whatever is still pending.
  bool advanceTo(int newTopRow) {
    if (!ok) return false;
    if (newTopRow <= bandStart) return true;
    if (newTopRow > height) newTopRow = height;
    // Room for another whole block? Then nothing has to move yet. maxBlockRows is what makes
    // this safe for a block-at-a-time decoder (JPEG MCU rows): the caller may write up to that
    // many rows starting at newTopRow, and they must all still land inside the band.
    if (newTopRow - bandStart + maxBlockRows <= bandRows) return true;
    return flushThrough(newTopRow);
  }

  // Stop a decode that will be resumed later (JpegToFramebufferConverter's checkpoint): write every
  // row below `finalRows` -- rows the decode will never touch again -- and close the partial file,
  // leaving it on the card for resume(). The band must hold nothing at or past `finalRows`, which
  // holds between two decode blocks. False (partial file dropped) when a write fails.
  bool park(int finalRows) {
    if (!ok) return false;
    if (finalRows > height) finalRows = height;
    if (finalRows < flushedRows || !flushThrough(finalRows)) {
      abort();
      return false;
    }
    file.close();
    ok = false;  // parked: the destructor leaves the partial file alone
    return true;
  }

  // Take up a parked cache: `rowsDone` rows are already in the partial file, and the decode goes
  // on from there. False when the partial file is missing or does not hold exactly those rows --
  // the caller then starts the whole decode over.
  bool resume(const std::string& cachePath, int w, int h, int ox, int oy, int maxBlockDstRows, int rowsDone,
              bool wasCoarse) {
    if (rowsDone < 0 || rowsDone > h) return false;
    if (!setUp(w, h, ox, oy, maxBlockDstRows)) return false;
    const std::string partPath = partPathFor(cachePath);
    if (!Storage.openFileForUpdate("IMG", partPath, file)) {
      dropBuffer();
      return false;
    }
    uint16_t header[3] = {};
    const size_t expected = PXC_HEADER_BYTES + static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowsDone);
    if (file.size() != expected || file.read(header, sizeof(header)) != static_cast<int>(sizeof(header)) ||
        header[0] != PXC_MAGIC || header[1] != static_cast<uint16_t>(w) || header[2] != static_cast<uint16_t>(h) ||
        !file.seekSet(expected)) {
      LOG_ERR("IMG", "Parked cache does not match its checkpoint: %s", partPath.c_str());
      file.close();
      dropBuffer();
      return false;
    }
    cachePathStr = cachePath;
    partPathStr = partPath;
    flushedRows = rowsDone;
    bandStart = rowsDone;
    coarse = wasCoarse;
    ok = true;
    return true;
  }

  // Flush the final band and fill any rows never covered (image clipped by the
  // screen, or a decode that produced fewer rows than the box), then close the file.
  bool finalize() {
    if (!ok) {
      abort();
      return false;
    }
    if (!flushThrough(height)) {
      abort();
      return false;
    }
    if (coarse) {
      const uint16_t magic = PXC_MAGIC_COARSE;
      if (!file.seekSet(0) || file.write(reinterpret_cast<const uint8_t*>(&magic), 2) != 2) {
        LOG_ERR("IMG", "Failed to stamp coarse pixel cache: %s", cachePathStr.c_str());
        abort();
        return false;
      }
    }
    file.close();
    // Publish. FAT rename does not overwrite, so a stale cache of the same name (a coarse one being
    // replaced) is removed first -- a readable file only: anything else at that path is not ours.
    if (!Storage.rename(partPathStr.c_str(), cachePathStr.c_str())) {
      bool staleFile = false;
      FsFile existing;
      if (Storage.openFileForRead("IMG", cachePathStr, existing)) {
        uint8_t probe = 0;
        staleFile = existing.read(&probe, 1) == 1;
        existing.close();
      }
      if (!staleFile || !Storage.remove(cachePathStr.c_str()) ||
          !Storage.rename(partPathStr.c_str(), cachePathStr.c_str())) {
        LOG_ERR("IMG", "Failed to publish pixel cache: %s", cachePathStr.c_str());
        Storage.remove(partPathStr.c_str());
        ok = false;
        return false;
      }
    }
    LOG_DBG("IMG", "Cache written: %s (%dx%d, %d bytes%s)", cachePathStr.c_str(), width, height,
            (int)PXC_HEADER_BYTES + bytesPerRow * height, coarse ? ", coarse" : "");
    ok = false;  // file handed off; nothing left to clean up
    return true;
  }

  // Drop a partial/failed cache so a later decode re-creates it cleanly.
  void abort() {
    if (file.isOpen()) file.close();
    if (!partPathStr.empty()) {
      Storage.remove(partPathStr.c_str());
    }
    ok = false;
  }

  ~PixelCache() {
    if (file.isOpen()) {
      // The file is still open, so neither finalize() nor abort() ran, or a
      // mid-stream write failed (advanceTo() cleared ok but left the file open).
      // Drop the partial cache so we leave no corrupt file behind.
      abort();
    }
    if (buffer) {
      free(buffer);
      buffer = nullptr;
    }
  }
};
