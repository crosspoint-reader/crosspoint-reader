#include "BmpToBmpConverter.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <BitmapHelpers.h>
#include <GifImageLayout.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <Memory.h>
#include <Print.h>

#include <algorithm>
#include <cstring>

namespace {

bool writeLE(Print& out, const uint32_t value, const int bytes) {
  for (int i = 0; i < bytes; ++i) {
    if (out.write(static_cast<uint8_t>(value >> (8 * i))) != 1) return false;
  }
  return true;
}

bool writeBmpHeader(Print& out, const int width, const int height, const bool topDown, const int bits,
                    const uint32_t rowBytes) {
  const uint32_t colors = 1u << bits;
  const uint32_t headerSize = 54 + colors * 4;
  const uint32_t imageSize = rowBytes * height;
  if (out.write('B') != 1 || out.write('M') != 1 || !writeLE(out, headerSize + imageSize, 4) || !writeLE(out, 0, 4) ||
      !writeLE(out, headerSize, 4) || !writeLE(out, 40, 4) || !writeLE(out, width, 4) ||
      !writeLE(out, static_cast<uint32_t>(topDown ? -height : height), 4) || !writeLE(out, 1, 2) ||
      !writeLE(out, bits, 2) || !writeLE(out, 0, 4) || !writeLE(out, imageSize, 4) || !writeLE(out, 2835, 4) ||
      !writeLE(out, 2835, 4) || !writeLE(out, colors, 4) || !writeLE(out, colors, 4)) {
    return false;
  }
  for (uint32_t i = 0; i < colors; ++i) {
    const uint32_t gray = i * 255 / (colors - 1);
    if (!writeLE(out, gray | gray << 8 | gray << 16, 4)) return false;
  }
  return true;
}

// Source position of output index i in 8.8 fixed point, pixel centers aligned.
int upscaleSource(const int i, const int dst, const int src) {
  return static_cast<int>(std::max<int64_t>(0, (static_cast<int64_t>(2 * i + 1) * src - dst) * 128 / dst));
}

}  // namespace

bool BmpToBmpConverter::bmpFileToBmpStream(HalFile& bmpFile, Print& bmpOut, const bool crop,
                                           const bool originalThresholds) {
  Bitmap src(bmpFile);
  const BmpReaderError err = src.parseHeaders();
  if (err != BmpReaderError::Ok) {
    LOG_ERR("BMP", "Invalid source BMP: %s", Bitmap::errorToString(err));
    return false;
  }

  // Display dimensions swapped for portrait, as for book covers.
  GifCommon::ImageLayout layout;
  if (!GifCommon::calculateLayout(src.getWidth(), src.getHeight(), display.getDisplayHeight(),
                                  display.getDisplayWidth(), crop, layout)) {
    LOG_ERR("BMP", "Invalid BMP layout %dx%d", src.getWidth(), src.getHeight());
    return false;
  }
  const int srcW = layout.sourceWidth;
  const int srcH = layout.sourceHeight;
  const int dstW = layout.width;
  const int dstH = layout.height;
  const bool upscale = dstW > srcW;
  const bool oneBit = src.is1Bit();
  const int bits = oneBit ? 1 : 2;
  const uint32_t outRowBytes = (static_cast<uint32_t>(dstW) * bits + 31) / 32 * 4;

  const size_t rawBytes = static_cast<size_t>(src.getRowBytes());
  const size_t grayBytes = static_cast<size_t>(src.getWidth()) * 2;  // two rows for bilinear upscaling
  auto raw = makeUniqueNoThrow<uint8_t[]>(rawBytes);
  auto gray = makeUniqueNoThrow<uint8_t[]>(grayBytes);
  auto accum = makeUniqueNoThrow<uint32_t[]>(dstW);
  auto outRow = makeUniqueNoThrow<uint8_t[]>(outRowBytes);
  if (!raw || !gray || !accum || !outRow) {
    LOG_ERR("BMP", "OOM: BMP conversion buffers (%u bytes)",
            static_cast<unsigned>(rawBytes + grayBytes + dstW * sizeof(uint32_t) + outRowBytes));
    return false;
  }
  std::unique_ptr<AtkinsonDitherer> dither2;
  std::unique_ptr<Atkinson1BitDitherer> dither1;
  if (oneBit) {
    dither1 = makeUniqueNoThrow<Atkinson1BitDitherer>(dstW);
  } else {
    dither2 = makeUniqueNoThrow<AtkinsonDitherer>(dstW, originalThresholds);
  }
  if (oneBit ? (!dither1 || !dither1->isValid()) : (!dither2 || !dither2->isValid())) {
    LOG_ERR("BMP", "OOM: BMP conversion ditherer (width %d)", dstW);
    return false;
  }

  // Emit rows in source file order so SD reads stay sequential.
  const bool topDown = src.isTopDown();
  if (!writeBmpHeader(bmpOut, dstW, dstH, topDown, bits, outRowBytes)) {
    LOG_ERR("BMP", "Failed to write BMP header");
    return false;
  }

  // Two cached source rows; a row still needed (`keep`) is never evicted.
  uint8_t* rows[2] = {gray.get(), gray.get() + src.getWidth()};
  int loaded[2] = {-1, -1};
  const auto rowFor = [&](const int y, const int keep) -> uint8_t* {
    if (loaded[0] == y) return rows[0];
    if (loaded[1] == y) return rows[1];
    const int slot = loaded[0] == keep ? 1 : 0;
    if (src.readRowGray(layout.sourceY + y, rows[slot], raw.get()) != BmpReaderError::Ok) {
      LOG_ERR("BMP", "Failed to read source row %d", y);
      return nullptr;
    }
    loaded[slot] = y;
    return rows[slot];
  };

  for (int i = 0; i < dstH; ++i) {
    const int outY = topDown ? i : dstH - 1 - i;
    if (upscale) {
      const int fy = upscaleSource(outY, dstH, srcH);
      const int y0 = std::min(fy >> 8, srcH - 1);
      const int y1 = std::min(y0 + 1, srcH - 1);
      const int wy = y0 < y1 ? fy & 0xFF : 0;
      const uint8_t* row0 = rowFor(y0, y1);
      const uint8_t* row1 = row0 ? rowFor(y1, y0) : nullptr;
      if (!row1) return false;
      for (int outX = 0; outX < dstW; ++outX) {
        const int fx = upscaleSource(outX, dstW, srcW);
        const int x0 = layout.sourceX + std::min(fx >> 8, srcW - 1);
        const int x1 = std::min(x0 + 1, layout.sourceX + srcW - 1);
        const int wx = fx & 0xFF;
        const int top = row0[x0] * (256 - wx) + row0[x1] * wx;
        const int bottom = row1[x0] * (256 - wx) + row1[x1] * wx;
        accum[outX] = (top * (256 - wy) + bottom * wy) >> 16;
      }
    } else {
      // Area average over the source box covered by this output row.
      const int y0 = outY * srcH / dstH;
      const int y1 = std::max(y0 + 1, (outY + 1) * srcH / dstH);
      memset(accum.get(), 0, dstW * sizeof(uint32_t));
      for (int k = y0; k < y1; ++k) {
        const uint8_t* row0 = rowFor(topDown ? k : y0 + y1 - 1 - k, -1);
        if (!row0) return false;
        for (int outX = 0; outX < dstW; ++outX) {
          const int x0 = outX * srcW / dstW;
          const int x1 = std::max(x0 + 1, (outX + 1) * srcW / dstW);
          uint32_t sum = 0;
          for (int x = x0; x < x1; ++x) sum += row0[layout.sourceX + x];
          accum[outX] += sum / (x1 - x0);
        }
      }
      for (int outX = 0; outX < dstW; ++outX) accum[outX] /= (y1 - y0);
    }

    memset(outRow.get(), 0, outRowBytes);
    for (int x = 0; x < dstW; ++x) {
      if (oneBit) {
        outRow[x / 8] |= dither1->processPixel(accum[x], x) << (7 - x % 8);
      } else {
        outRow[x / 4] |= dither2->processPixel(adjustPixel(accum[x]), x) << (6 - (x % 4) * 2);
      }
    }
    if (oneBit)
      dither1->nextRow();
    else
      dither2->nextRow();

    if (bmpOut.write(outRow.get(), outRowBytes) != outRowBytes) {
      LOG_ERR("BMP", "Short BMP row write");
      return false;
    }
    if ((i & 63) == 63) vTaskDelay(1);
  }
  return true;
}
