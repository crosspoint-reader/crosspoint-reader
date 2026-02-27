#include "ScreenCapture.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

// Writes a little-endian value to a file.
namespace {
void write16(FsFile& f, uint16_t v) { f.write(reinterpret_cast<const uint8_t*>(&v), 2); }
void write32(FsFile& f, uint32_t v) { f.write(reinterpret_cast<const uint8_t*>(&v), 4); }
void write32s(FsFile& f, int32_t v) { f.write(reinterpret_cast<const uint8_t*>(&v), 4); }

// Output portrait BMP dimensions (after 270° CCW rotation of the 800×480 landscape buffer)
constexpr int OUT_W = EInkDisplay::DISPLAY_HEIGHT;  // 480
constexpr int OUT_H = EInkDisplay::DISPLAY_WIDTH;   // 800

// Source landscape buffer dimensions
constexpr int SRC_W = EInkDisplay::DISPLAY_WIDTH;           // 800
constexpr int SRC_H = EInkDisplay::DISPLAY_HEIGHT;          // 480
constexpr int SRC_BYTES_PER_ROW = EInkDisplay::DISPLAY_WIDTH_BYTES;  // 100

// Output 1-bit BMP row width (480 px / 8 = 60 bytes, already 4-byte aligned)
constexpr int OUT_BYTES_PER_ROW = OUT_W / 8;  // = 60
constexpr uint32_t IMAGE_SIZE = static_cast<uint32_t>(OUT_BYTES_PER_ROW) * OUT_H;  // 48,000

void writeBmpHeader(FsFile& f) {
  constexpr uint32_t FILE_SIZE = 14 + 40 + 8 + IMAGE_SIZE;

  // BMP file header (14 bytes)
  f.write('B');
  f.write('M');
  write32(f, FILE_SIZE);
  write32(f, 0);            // reserved
  write32(f, 14 + 40 + 8); // offset to pixel data

  // DIB header / BITMAPINFOHEADER (40 bytes)
  write32(f, 40);           // header size
  write32s(f, OUT_W);       // image width  (480)
  write32s(f, -OUT_H);      // image height (-800, negative = top-down row order)
  write16(f, 1);            // colour planes
  write16(f, 1);            // bits per pixel
  write32(f, 0);            // compression (BI_RGB)
  write32(f, IMAGE_SIZE);   // raw image size
  write32(f, 2835);         // x pixels/metre (≈72 dpi)
  write32(f, 2835);         // y pixels/metre
  write32(f, 2);            // colours in table
  write32(f, 2);            // important colours

  // 2-colour palette (8 bytes)
  // Index 0 = black, index 1 = white  (matches frame buffer: 0=black, 1=white)
  const uint8_t palette[8] = {
      0x00, 0x00, 0x00, 0x00,  // palette[0]: black  (BGRA)
      0xFF, 0xFF, 0xFF, 0x00,  // palette[1]: white  (BGRA)
  };
  f.write(palette, 8);
}

// Rotate the landscape frame buffer 90° CW and write pixel data row by row to the file.
//
// Rotation formula: output pixel (ox, oy) comes from source pixel (srcX = oy, srcY = SRC_H-1-ox).
//   - The leftmost column (srcX = 0) of the landscape buffer becomes the top row of
//     the portrait output.
void writeRotatedPixels(FsFile& f, const uint8_t* src) {
  uint8_t rowBuf[OUT_BYTES_PER_ROW];

  for (int oy = 0; oy < OUT_H; oy++) {
    // All pixels in this output row share the same source column.
    const int srcX    = oy;                        // source column (0..799)
    const int srcBOff = srcX / 8;                  // byte offset within source row
    const int srcShft = 7 - (srcX % 8);            // bit position within that byte

    // Start with all-white output row, then punch in black pixels.
    memset(rowBuf, 0xFF, OUT_BYTES_PER_ROW);

    for (int ox = 0; ox < OUT_W; ox++) {
      // srcY = SRC_H-1-ox (source row index = bottom-up output column)
      const uint8_t bit = (src[(SRC_H - 1 - ox) * SRC_BYTES_PER_ROW + srcBOff] >> srcShft) & 1;
      if (bit == 0) {
        // Black pixel: clear the corresponding bit in the output byte (MSB-first)
        rowBuf[ox / 8] &= ~static_cast<uint8_t>(1 << (7 - (ox % 8)));
      }
    }

    f.write(rowBuf, OUT_BYTES_PER_ROW);
  }
}

}  // namespace

bool ScreenCapture::save(const GfxRenderer& renderer, const char* name) {
  Storage.mkdir("/screencap");

  char path[64];
  snprintf(path, sizeof(path), "/screencap/%s.bmp", name);

  FsFile file;
  if (!Storage.openFileForWrite("SCAP", path, file)) {
    LOG_ERR("SCAP", "Failed to open %s for writing", path);
    return false;
  }

  writeBmpHeader(file);
  writeRotatedPixels(file, renderer.getFrameBuffer());
  file.close();

  LOG_INF("SCAP", "Saved %s", path);
  return true;
}
