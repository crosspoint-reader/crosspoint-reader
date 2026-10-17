#include "PngStreamDecoder.h"

#include <Logging.h>

#include <cstdlib>
#include <cstring>

namespace {

constexpr uint8_t PNG_SIGNATURE[8] = {137, 80, 78, 71, 13, 10, 26, 10};

enum PngColorType : uint8_t {
  PNG_COLOR_GRAYSCALE = 0,
  PNG_COLOR_RGB = 2,
  PNG_COLOR_PALETTE = 3,
  PNG_COLOR_GRAYSCALE_ALPHA = 4,
  PNG_COLOR_RGBA = 6,
};

enum PngFilter : uint8_t {
  PNG_FILTER_NONE = 0,
  PNG_FILTER_SUB = 1,
  PNG_FILTER_UP = 2,
  PNG_FILTER_AVERAGE = 3,
  PNG_FILTER_PAETH = 4,
};

// Luma weights matching the rest of the image pipeline (0.30/0.586/0.114).
inline uint8_t rgbToGray(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
}

inline uint8_t compositeOverWhite(uint8_t value, uint8_t alpha) {
  if (alpha == 255) return value;
  if (alpha == 0) return 255;
  return static_cast<uint8_t>((value * alpha + 255 * (255 - alpha)) / 255);
}

inline uint8_t paethPredictor(uint8_t a, uint8_t b, uint8_t c) {
  int p = static_cast<int>(a) + b - c;
  int pa = p > a ? p - a : a - p;
  int pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

bool readBE32(FsFile& file, uint32_t& value) {
  uint8_t buf[4];
  if (file.read(buf, 4) != 4) return false;
  value = (static_cast<uint32_t>(buf[0]) << 24) | (static_cast<uint32_t>(buf[1]) << 16) |
          (static_cast<uint32_t>(buf[2]) << 8) | buf[3];
  return true;
}

// PNG spec table 11.1: the bit depths each colour type allows. Any other pairing is a corrupt
// IHDR that nextRow() cannot unpack: a grayscale/palette depth of 3/5/6/7 indexes past the end
// of the row buffer, and a sub-byte path with a depth above 8 (grayscale 9-15, palette 16) makes
// ppb = 8 / bitDepth zero and divides by it. RISC-V does not trap that division, so a corrupt
// EPUB image read garbage memory instead of being rejected.
// Ported from crosspoint-reader PR #3979 (phucisstupid).
bool isValidBitDepth(const uint8_t colorType, const uint8_t bitDepth) {
  switch (colorType) {
    case PNG_COLOR_GRAYSCALE:
      return bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8 || bitDepth == 16;
    case PNG_COLOR_PALETTE:
      return bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8;
    case PNG_COLOR_RGB:
    case PNG_COLOR_GRAYSCALE_ALPHA:
    case PNG_COLOR_RGBA:
      return bitDepth == 8 || bitDepth == 16;
    default:
      return false;
  }
}

constexpr int MAX_IMAGE_WIDTH = 2048;
constexpr int MAX_IMAGE_HEIGHT = 3072;
constexpr uint32_t MAX_RAW_ROW_BYTES = 16384;

}  // namespace

PngStreamDecoder::~PngStreamDecoder() { end(); }

size_t PngStreamDecoder::scratchBytesFor(const size_t expectedOutputSize, const uint32_t rawRowBytes) {
  // Ring + two scanline buffers, plus per-allocation alignment slack so a caller sizing an
  // arena from this never comes up short by a few padding bytes.
  constexpr size_t kAlignSlack = 3 * alignof(std::max_align_t);
  return InflateReader::ringSizeFor(expectedOutputSize) + 2 * static_cast<size_t>(rawRowBytes) + kAlignSlack;
}

void PngStreamDecoder::end() {
  reader_.deinit();  // no-op on the ring when it was supplied externally (ownsRing_ == false)
  if (!arenaOwnsBuffers_) {
    free(currentRow_);
    free(previousRow_);
  }
  currentRow_ = nullptr;
  previousRow_ = nullptr;
  // Release the whole scope in one go. Newest-first is satisfied because the decoder reserves
  // and releases within its own lifetime and nothing else allocates from the arena in between.
  if (scratchBlock_.valid()) {
    scratchArena_->release(scratchBlock_);
  }
  arenaOwnsBuffers_ = false;
  started_ = false;
}

bool PngStreamDecoder::begin(FsFile& file, Info& info) {
  file_ = &file;
  rowsProduced_ = 0;
  paletteSize_ = 0;
  hasPaletteAlpha_ = false;
  hasColorKey_ = false;
  idatFinished_ = false;
  chunkBytesRemaining_ = 0;
  memset(paletteAlpha_, 255, sizeof(paletteAlpha_));

  // Signature + IHDR
  uint8_t sig[8];
  if (file.read(sig, 8) != 8 || memcmp(sig, PNG_SIGNATURE, 8) != 0) {
    LOG_ERR("PNG", "Invalid PNG signature");
    return false;
  }

  uint32_t ihdrLen;
  if (!readBE32(file, ihdrLen)) return false;
  uint8_t ihdrType[4];
  if (file.read(ihdrType, 4) != 4 || memcmp(ihdrType, "IHDR", 4) != 0) {
    LOG_ERR("PNG", "Missing IHDR chunk");
    return false;
  }
  if (!readBE32(file, width_) || !readBE32(file, height_)) return false;
  uint8_t ihdrRest[5];
  if (file.read(ihdrRest, 5) != 5) return false;
  bitDepth_ = ihdrRest[0];
  colorType_ = ihdrRest[1];
  const uint8_t compression = ihdrRest[2];
  const uint8_t filter = ihdrRest[3];
  const uint8_t interlace = ihdrRest[4];
  file.seekCur(4);  // IHDR CRC

  if (compression != 0 || filter != 0) {
    LOG_ERR("PNG", "Unsupported compression/filter method");
    return false;
  }
  if (interlace != 0) {
    LOG_ERR("PNG", "Interlaced PNGs not supported");
    return false;
  }
  if (width_ == 0 || height_ == 0 || width_ > MAX_IMAGE_WIDTH || height_ > MAX_IMAGE_HEIGHT) {
    LOG_ERR("PNG", "Image too large or zero (%ux%u)", width_, height_);
    return false;
  }

  // Bytes per pixel (for the reverse filter) and raw row length.
  switch (colorType_) {
    case PNG_COLOR_GRAYSCALE:
      bytesPerPixel_ = (bitDepth_ == 16) ? 2 : 1;
      rawRowBytes_ = (bitDepth_ >= 8) ? width_ * bytesPerPixel_ : (width_ * bitDepth_ + 7) / 8;
      break;
    case PNG_COLOR_RGB:
      bytesPerPixel_ = (bitDepth_ == 16) ? 6 : 3;
      rawRowBytes_ = width_ * bytesPerPixel_;
      break;
    case PNG_COLOR_PALETTE:
      bytesPerPixel_ = 1;
      rawRowBytes_ = (width_ * bitDepth_ + 7) / 8;
      break;
    case PNG_COLOR_GRAYSCALE_ALPHA:
      bytesPerPixel_ = (bitDepth_ == 16) ? 4 : 2;
      rawRowBytes_ = width_ * bytesPerPixel_;
      break;
    case PNG_COLOR_RGBA:
      bytesPerPixel_ = (bitDepth_ == 16) ? 8 : 4;
      rawRowBytes_ = width_ * bytesPerPixel_;
      break;
    default:
      LOG_ERR("PNG", "Unsupported color type: %d", colorType_);
      return false;
  }
  if (!isValidBitDepth(colorType_, bitDepth_)) {
    LOG_ERR("PNG", "Invalid bit depth %d for color type %d", bitDepth_, colorType_);
    return false;
  }
  if (rawRowBytes_ == 0 || rawRowBytes_ > MAX_RAW_ROW_BYTES) {
    LOG_ERR("PNG", "Row too large: %u bytes", rawRowBytes_);
    return false;
  }

  // Scanline buffers: from the scratch arena when one was supplied and both fit, else the heap.
  // Both must come from the same source — a half-arena pair would make end() free an arena
  // pointer. The ring is taken later (it needs height_, parsed below) from the same block.
  if (scratchArena_ && scratchArena_->valid()) {
    scratchBlock_ = scratchArena_->reserveBlock();
    if (scratchBlock_.valid()) {
      auto* cur = static_cast<uint8_t*>(scratchArena_->alloc(rawRowBytes_));
      auto* prev = static_cast<uint8_t*>(scratchArena_->alloc(rawRowBytes_));
      if (cur && prev) {
        currentRow_ = cur;
        previousRow_ = prev;
        memset(previousRow_, 0, rawRowBytes_);  // calloc equivalent: the filter reads row -1
        arenaOwnsBuffers_ = true;
      } else {
        // Arena too small for this image — hand the scope straight back and use the heap.
        scratchArena_->release(scratchBlock_);
      }
    }
  }
  if (!arenaOwnsBuffers_) {
    currentRow_ = static_cast<uint8_t*>(malloc(rawRowBytes_));
    previousRow_ = static_cast<uint8_t*>(calloc(rawRowBytes_, 1));
    if (!currentRow_ || !previousRow_) {
      LOG_ERR("PNG", "Failed to allocate scanline buffers (%u bytes each)", rawRowBytes_);
      end();
      return false;
    }
  }

  // Walk chunks collecting PLTE / tRNS until the first IDAT.
  bool foundIdat = false;
  while (!foundIdat) {
    uint32_t chunkLen;
    if (!readBE32(file, chunkLen)) break;
    uint8_t chunkType[4];
    if (file.read(chunkType, 4) != 4) break;

    if (memcmp(chunkType, "PLTE", 4) == 0) {
      int entries = chunkLen / 3;
      if (entries > 256) entries = 256;
      paletteSize_ = entries;
      const size_t palBytes = static_cast<size_t>(entries) * 3;
      file.read(palette_, palBytes);
      if (chunkLen > palBytes) file.seekCur(chunkLen - palBytes);
      file.seekCur(4);  // CRC
    } else if (memcmp(chunkType, "tRNS", 4) == 0) {
      if (colorType_ == PNG_COLOR_PALETTE) {
        uint32_t n = chunkLen;
        if (n > 256) n = 256;
        file.read(paletteAlpha_, n);  // remaining entries stay 255 (opaque)
        if (chunkLen > n) file.seekCur(chunkLen - n);
        hasPaletteAlpha_ = true;
      } else if (colorType_ == PNG_COLOR_GRAYSCALE && chunkLen >= 2) {
        uint8_t b[2];
        file.read(b, 2);
        keyGray_ = (static_cast<uint16_t>(b[0]) << 8) | b[1];
        if (chunkLen > 2) file.seekCur(chunkLen - 2);
        hasColorKey_ = true;
      } else if (colorType_ == PNG_COLOR_RGB && chunkLen >= 6) {
        uint8_t b[6];
        file.read(b, 6);
        keyR_ = (static_cast<uint16_t>(b[0]) << 8) | b[1];
        keyG_ = (static_cast<uint16_t>(b[2]) << 8) | b[3];
        keyB_ = (static_cast<uint16_t>(b[4]) << 8) | b[5];
        if (chunkLen > 6) file.seekCur(chunkLen - 6);
        hasColorKey_ = true;
      } else {
        file.seekCur(chunkLen);
      }
      file.seekCur(4);  // CRC
    } else if (memcmp(chunkType, "IDAT", 4) == 0) {
      chunkBytesRemaining_ = chunkLen;
      foundIdat = true;
    } else if (memcmp(chunkType, "IEND", 4) == 0) {
      break;
    } else {
      file.seekCur(chunkLen + 4);  // skip data + CRC
    }
  }

  if (!foundIdat) {
    LOG_ERR("PNG", "No IDAT chunk found");
    end();
    return false;
  }

  // A deflate back-reference can never point further back than the bytes
  // produced so far, so cap the ring at the exact uncompressed size (filter
  // byte + raw bytes per row). Small images thus pay only a few KB.
  const size_t uncompressed = static_cast<size_t>(height_) * (rawRowBytes_ + 1);
  // The ring is the largest single block on this path (up to 32 KB). Take it from the same
  // arena scope as the scanline buffers when available — initWithExternalRing leaves ownership
  // with us, so deinit() will not free arena memory.
  bool ringReady = false;
  if (arenaOwnsBuffers_) {
    const size_t ringSize = InflateReader::ringSizeFor(uncompressed);
    if (auto* ring = static_cast<uint8_t*>(scratchArena_->alloc(ringSize))) {
      memset(ring, 0, ringSize);
      ringReady = reader_.initWithExternalRing(ring, ringSize);
    }
    // No arena room for the ring: fall through to the heap. The scanline buffers stay in the
    // arena — they are already live and the block is released as a whole by end().
  }
  if (!ringReady && !reader_.init(true, uncompressed)) {
    // The ring CANNOT be retried smaller: ringSizeFor's contract is "total output <= ring size
    // guarantees no back-reference can outrun the ring", so a short ring on a large image would
    // silently produce corrupt pixels. Failing is correct — but say why, loudly. This path is
    // how an image disappears from a page with no user-visible explanation (observed on X3 at
    // contig~30 KB), and "Failed to init inflate reader" gave no clue that it was a contiguous-
    // memory problem rather than a malformed PNG.
    LOG_ERR("PNG", "Inflate ring alloc failed: need %u contiguous bytes (scratch arena: %s); image dropped",
            static_cast<unsigned>(InflateReader::ringSizeFor(uncompressed)),
            arenaOwnsBuffers_ ? "present but too small" : "none supplied");
    end();
    return false;
  }
  reader_.setReadCallback(idatReadCallback);
  reader_.skipZlibHeader();  // PNG IDAT is a zlib stream (CMF + FLG)

  info.width = width_;
  info.height = height_;
  info.bitDepth = bitDepth_;
  info.colorType = colorType_;
  started_ = true;
  return true;
}

bool PngStreamDecoder::findNextIdatChunk() {
  while (true) {
    uint32_t chunkLen;
    if (!readBE32(*file_, chunkLen)) return false;
    uint8_t chunkType[4];
    if (file_->read(chunkType, 4) != 4) return false;
    if (memcmp(chunkType, "IDAT", 4) == 0) {
      chunkBytesRemaining_ = chunkLen;
      return true;
    }
    if (!file_->seekCur(chunkLen + 4)) return false;  // skip data + CRC
    if (memcmp(chunkType, "IEND", 4) == 0) return false;
  }
}

int PngStreamDecoder::idatReadCallback(uzlib_uncomp* uncomp) {
  auto* self = reinterpret_cast<PngStreamDecoder*>(uncomp);
  if (self->idatFinished_) return -1;

  while (self->chunkBytesRemaining_ == 0) {
    if (!self->file_->seekCur(4)) {  // skip CRC of the IDAT just consumed
      self->idatFinished_ = true;
      return -1;
    }
    if (!self->findNextIdatChunk()) {
      self->idatFinished_ = true;
      return -1;
    }
  }

  size_t toRead = sizeof(self->readBuf_);
  if (toRead > self->chunkBytesRemaining_) toRead = self->chunkBytesRemaining_;
  const int bytesRead = self->file_->read(self->readBuf_, toRead);
  if (bytesRead <= 0) {
    self->idatFinished_ = true;
    return -1;
  }
  self->chunkBytesRemaining_ -= bytesRead;

  // Hand uzlib the buffer; return the first byte directly per its callback ABI.
  uncomp->source = self->readBuf_ + 1;
  uncomp->source_limit = self->readBuf_ + bytesRead;
  return self->readBuf_[0];
}

bool PngStreamDecoder::decodeFilteredRow() {
  uint8_t filterType;
  if (!reader_.read(&filterType, 1)) return false;
  if (!reader_.read(currentRow_, rawRowBytes_)) return false;

  const int bpp = bytesPerPixel_;
  switch (filterType) {
    case PNG_FILTER_NONE:
      break;
    case PNG_FILTER_SUB:
      for (uint32_t i = bpp; i < rawRowBytes_; i++) currentRow_[i] += currentRow_[i - bpp];
      break;
    case PNG_FILTER_UP:
      for (uint32_t i = 0; i < rawRowBytes_; i++) currentRow_[i] += previousRow_[i];
      break;
    case PNG_FILTER_AVERAGE:
      for (uint32_t i = 0; i < rawRowBytes_; i++) {
        const uint8_t a = (i >= static_cast<uint32_t>(bpp)) ? currentRow_[i - bpp] : 0;
        currentRow_[i] += (a + previousRow_[i]) / 2;
      }
      break;
    case PNG_FILTER_PAETH:
      for (uint32_t i = 0; i < rawRowBytes_; i++) {
        const uint8_t a = (i >= static_cast<uint32_t>(bpp)) ? currentRow_[i - bpp] : 0;
        const uint8_t b = previousRow_[i];
        const uint8_t c = (i >= static_cast<uint32_t>(bpp)) ? previousRow_[i - bpp] : 0;
        currentRow_[i] += paethPredictor(a, b, c);
      }
      break;
    default:
      LOG_ERR("PNG", "Unknown filter type: %d", filterType);
      return false;
  }
  return true;
}

void PngStreamDecoder::finishRow() {
  uint8_t* tmp = previousRow_;
  previousRow_ = currentRow_;
  currentRow_ = tmp;
  rowsProduced_++;
}

bool PngStreamDecoder::skipRow() {
  if (!started_ || rowsProduced_ >= height_ || !decodeFilteredRow()) return false;
  finishRow();
  return true;
}

bool PngStreamDecoder::nextRow(uint8_t* grayOut, uint8_t* alphaOut) {
  if (!started_ || rowsProduced_ >= height_) return false;
  if (!decodeFilteredRow()) {
    LOG_ERR("PNG", "Failed to decode scanline %u", rowsProduced_);
    return false;
  }

  const uint8_t* src = currentRow_;
  const uint32_t w = width_;
  const bool wantAlpha = (alphaOut != nullptr);

  switch (colorType_) {
    case PNG_COLOR_GRAYSCALE: {
      if (bitDepth_ == 8) {
        for (uint32_t x = 0; x < w; x++) {
          const uint8_t v = src[x];
          const uint8_t a = (hasColorKey_ && v == (keyGray_ & 0xFF)) ? 0 : 255;
          if (wantAlpha) {
            grayOut[x] = v;
            alphaOut[x] = a;
          } else {
            grayOut[x] = compositeOverWhite(v, a);
          }
        }
      } else if (bitDepth_ == 16) {
        for (uint32_t x = 0; x < w; x++) {
          const uint16_t sample = (static_cast<uint16_t>(src[x * 2]) << 8) | src[x * 2 + 1];
          const uint8_t v = src[x * 2];  // high byte
          const uint8_t a = (hasColorKey_ && sample == keyGray_) ? 0 : 255;
          if (wantAlpha) {
            grayOut[x] = v;
            alphaOut[x] = a;
          } else {
            grayOut[x] = compositeOverWhite(v, a);
          }
        }
      } else {
        const int ppb = 8 / bitDepth_;
        const uint8_t mask = (1 << bitDepth_) - 1;
        for (uint32_t x = 0; x < w; x++) {
          const int shift = (ppb - 1 - (x % ppb)) * bitDepth_;
          const uint8_t sample = (src[x / ppb] >> shift) & mask;
          const uint8_t v = static_cast<uint8_t>(sample * 255 / mask);
          const uint8_t a = (hasColorKey_ && sample == (keyGray_ & mask)) ? 0 : 255;
          if (wantAlpha) {
            grayOut[x] = v;
            alphaOut[x] = a;
          } else {
            grayOut[x] = compositeOverWhite(v, a);
          }
        }
      }
      break;
    }

    case PNG_COLOR_RGB: {
      const int stride = (bitDepth_ == 16) ? 6 : 3;
      for (uint32_t x = 0; x < w; x++) {
        const uint8_t* p = src + x * stride;
        const uint8_t r = p[0], g = (bitDepth_ == 16) ? p[2] : p[1], b = (bitDepth_ == 16) ? p[4] : p[2];
        const uint8_t v = rgbToGray(r, g, b);
        uint8_t a = 255;
        if (hasColorKey_) {
          // Compare at native depth: for 8-bit the key's low byte holds the sample.
          if (bitDepth_ == 16) {
            const uint16_t pr = (static_cast<uint16_t>(p[0]) << 8) | p[1];
            const uint16_t pg = (static_cast<uint16_t>(p[2]) << 8) | p[3];
            const uint16_t pb = (static_cast<uint16_t>(p[4]) << 8) | p[5];
            if (pr == keyR_ && pg == keyG_ && pb == keyB_) a = 0;
          } else if (r == (keyR_ & 0xFF) && g == (keyG_ & 0xFF) && b == (keyB_ & 0xFF)) {
            a = 0;
          }
        }
        if (wantAlpha) {
          grayOut[x] = v;
          alphaOut[x] = a;
        } else {
          grayOut[x] = compositeOverWhite(v, a);
        }
      }
      break;
    }

    case PNG_COLOR_PALETTE: {
      const int ppb = 8 / bitDepth_;
      const uint8_t mask = (1 << bitDepth_) - 1;
      for (uint32_t x = 0; x < w; x++) {
        const int shift = (ppb - 1 - (x % ppb)) * bitDepth_;
        uint8_t idx = (src[x / ppb] >> shift) & mask;
        if (idx >= paletteSize_) idx = 0;
        const uint8_t* p = palette_ + idx * 3;
        const uint8_t v = rgbToGray(p[0], p[1], p[2]);
        const uint8_t a = hasPaletteAlpha_ ? paletteAlpha_[idx] : 255;
        if (wantAlpha) {
          grayOut[x] = v;
          alphaOut[x] = a;
        } else {
          grayOut[x] = compositeOverWhite(v, a);
        }
      }
      break;
    }

    case PNG_COLOR_GRAYSCALE_ALPHA: {
      const int stride = (bitDepth_ == 16) ? 4 : 2;
      for (uint32_t x = 0; x < w; x++) {
        const uint8_t v = src[x * stride];
        const uint8_t a = (bitDepth_ == 16) ? src[x * stride + 2] : src[x * stride + 1];
        if (wantAlpha) {
          grayOut[x] = v;
          alphaOut[x] = a;
        } else {
          grayOut[x] = compositeOverWhite(v, a);
        }
      }
      break;
    }

    case PNG_COLOR_RGBA: {
      const int stride = (bitDepth_ == 16) ? 8 : 4;
      for (uint32_t x = 0; x < w; x++) {
        const uint8_t* p = src + x * stride;
        const uint8_t r = p[0], g = (bitDepth_ == 16) ? p[2] : p[1], b = (bitDepth_ == 16) ? p[4] : p[2];
        const uint8_t v = rgbToGray(r, g, b);
        const uint8_t a = (bitDepth_ == 16) ? p[6] : p[3];
        if (wantAlpha) {
          grayOut[x] = v;
          alphaOut[x] = a;
        } else {
          grayOut[x] = compositeOverWhite(v, a);
        }
      }
      break;
    }

    default:
      memset(grayOut, 128, w);
      if (wantAlpha) memset(alphaOut, 255, w);
      break;
  }

  finishRow();
  return true;
}
