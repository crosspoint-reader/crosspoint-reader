#pragma once
// ============================================================
// BitmapRenderer — draws to a real pixel buffer, saves as BMP
// Drop-in replacement for GfxRenderer in preview tests
// ============================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>

namespace EpdFontFamily {
  enum Style { REGULAR = 0, BOLD = 1, ITALIC = 2 };
}

namespace HalDisplay {
  enum RefreshMode { FAST_REFRESH = 0, HALF_REFRESH = 1, FULL_REFRESH = 2 };
}

struct Rect {
  int x, y, w, h;
  Rect() : x(0), y(0), w(0), h(0) {}
  Rect(int x, int y, int w, int h) : x(x), y(y), w(w), h(h) {}
};

class GfxRenderer {
 public:
  static constexpr int SW = 480;
  static constexpr int SH = 800;
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };

 private:
  uint8_t fb[SW * SH];  // 0=white, 1=black
  // Simple 8x16 bitmap font (ASCII 32-126)
  // Each char: 8 wide x 16 tall, stored as 16 bytes (1 bit per pixel, MSB left)
  static constexpr int CHAR_W = 8;
  static constexpr int CHAR_H = 16;

  void setPixel(int x, int y, bool black) {
    if (x >= 0 && x < SW && y >= 0 && y < SH)
      fb[y * SW + x] = black ? 1 : 0;
  }

  bool getPixel(int x, int y) const {
    if (x >= 0 && x < SW && y >= 0 && y < SH)
      return fb[y * SW + x] != 0;
    return false;
  }

  // Render a character using a simple procedural font
  void drawChar(int x, int y, char c, bool black, int scale) {
    if (c < 32 || c > 126) c = '?';
    // Simple 5x7 font embedded procedurally
    // For readability, we draw using basic strokes
    for (int row = 0; row < 7; row++) {
      for (int col = 0; col < 5; col++) {
        if (getGlyphPixel(c, col, row)) {
          for (int sy = 0; sy < scale; sy++)
            for (int sx = 0; sx < scale; sx++)
              setPixel(x + col * scale + sx, y + row * scale + sy, black);
        }
      }
    }
  }

  // Minimal 5x7 procedural font — covers ASCII printable range
  static bool getGlyphPixel(char c, int x, int y) {
    // Each glyph: 5 columns x 7 rows packed into uint8_t[5] (one byte per column, 7 bits used)
    static const uint8_t font[][5] = {
      {0x00,0x00,0x00,0x00,0x00}, // 32 space
      {0x00,0x00,0x5F,0x00,0x00}, // 33 !
      {0x00,0x07,0x00,0x07,0x00}, // 34 "
      {0x14,0x7F,0x14,0x7F,0x14}, // 35 #
      {0x24,0x2A,0x7F,0x2A,0x12}, // 36 $
      {0x23,0x13,0x08,0x64,0x62}, // 37 %
      {0x36,0x49,0x55,0x22,0x50}, // 38 &
      {0x00,0x05,0x03,0x00,0x00}, // 39 '
      {0x00,0x1C,0x22,0x41,0x00}, // 40 (
      {0x00,0x41,0x22,0x1C,0x00}, // 41 )
      {0x08,0x2A,0x1C,0x2A,0x08}, // 42 *
      {0x08,0x08,0x3E,0x08,0x08}, // 43 +
      {0x00,0x50,0x30,0x00,0x00}, // 44 ,
      {0x08,0x08,0x08,0x08,0x08}, // 45 -
      {0x00,0x60,0x60,0x00,0x00}, // 46 .
      {0x20,0x10,0x08,0x04,0x02}, // 47 /
      {0x3E,0x51,0x49,0x45,0x3E}, // 48 0
      {0x00,0x42,0x7F,0x40,0x00}, // 49 1
      {0x42,0x61,0x51,0x49,0x46}, // 50 2
      {0x21,0x41,0x45,0x4B,0x31}, // 51 3
      {0x18,0x14,0x12,0x7F,0x10}, // 52 4
      {0x27,0x45,0x45,0x45,0x39}, // 53 5
      {0x3C,0x4A,0x49,0x49,0x30}, // 54 6
      {0x01,0x71,0x09,0x05,0x03}, // 55 7
      {0x36,0x49,0x49,0x49,0x36}, // 56 8
      {0x06,0x49,0x49,0x29,0x1E}, // 57 9
      {0x00,0x36,0x36,0x00,0x00}, // 58 :
      {0x00,0x56,0x36,0x00,0x00}, // 59 ;
      {0x00,0x08,0x14,0x22,0x41}, // 60 <
      {0x14,0x14,0x14,0x14,0x14}, // 61 =
      {0x41,0x22,0x14,0x08,0x00}, // 62 >
      {0x02,0x01,0x51,0x09,0x06}, // 63 ?
      {0x32,0x49,0x79,0x41,0x3E}, // 64 @
      {0x7E,0x11,0x11,0x11,0x7E}, // 65 A
      {0x7F,0x49,0x49,0x49,0x36}, // 66 B
      {0x3E,0x41,0x41,0x41,0x22}, // 67 C
      {0x7F,0x41,0x41,0x22,0x1C}, // 68 D
      {0x7F,0x49,0x49,0x49,0x41}, // 69 E
      {0x7F,0x09,0x09,0x01,0x01}, // 70 F
      {0x3E,0x41,0x41,0x51,0x32}, // 71 G
      {0x7F,0x08,0x08,0x08,0x7F}, // 72 H
      {0x00,0x41,0x7F,0x41,0x00}, // 73 I
      {0x20,0x40,0x41,0x3F,0x01}, // 74 J
      {0x7F,0x08,0x14,0x22,0x41}, // 75 K
      {0x7F,0x40,0x40,0x40,0x40}, // 76 L
      {0x7F,0x02,0x04,0x02,0x7F}, // 77 M
      {0x7F,0x04,0x08,0x10,0x7F}, // 78 N
      {0x3E,0x41,0x41,0x41,0x3E}, // 79 O
      {0x7F,0x09,0x09,0x09,0x06}, // 80 P
      {0x3E,0x41,0x51,0x21,0x5E}, // 81 Q
      {0x7F,0x09,0x19,0x29,0x46}, // 82 R
      {0x46,0x49,0x49,0x49,0x31}, // 83 S
      {0x01,0x01,0x7F,0x01,0x01}, // 84 T
      {0x3F,0x40,0x40,0x40,0x3F}, // 85 U
      {0x1F,0x20,0x40,0x20,0x1F}, // 86 V
      {0x7F,0x20,0x18,0x20,0x7F}, // 87 W
      {0x63,0x14,0x08,0x14,0x63}, // 88 X
      {0x03,0x04,0x78,0x04,0x03}, // 89 Y
      {0x61,0x51,0x49,0x45,0x43}, // 90 Z
      {0x00,0x00,0x7F,0x41,0x41}, // 91 [
      {0x02,0x04,0x08,0x10,0x20}, // 92 backslash
      {0x41,0x41,0x7F,0x00,0x00}, // 93 ]
      {0x04,0x02,0x01,0x02,0x04}, // 94 ^
      {0x40,0x40,0x40,0x40,0x40}, // 95 _
      {0x00,0x01,0x02,0x04,0x00}, // 96 `
      {0x20,0x54,0x54,0x54,0x78}, // 97 a
      {0x7F,0x48,0x44,0x44,0x38}, // 98 b
      {0x38,0x44,0x44,0x44,0x20}, // 99 c
      {0x38,0x44,0x44,0x48,0x7F}, // 100 d
      {0x38,0x54,0x54,0x54,0x18}, // 101 e
      {0x08,0x7E,0x09,0x01,0x02}, // 102 f
      {0x08,0x14,0x54,0x54,0x3C}, // 103 g
      {0x7F,0x08,0x04,0x04,0x78}, // 104 h
      {0x00,0x44,0x7D,0x40,0x00}, // 105 i
      {0x20,0x40,0x44,0x3D,0x00}, // 106 j
      {0x00,0x7F,0x10,0x28,0x44}, // 107 k
      {0x00,0x41,0x7F,0x40,0x00}, // 108 l
      {0x7C,0x04,0x18,0x04,0x78}, // 109 m
      {0x7C,0x08,0x04,0x04,0x78}, // 110 n
      {0x38,0x44,0x44,0x44,0x38}, // 111 o
      {0x7C,0x14,0x14,0x14,0x08}, // 112 p
      {0x08,0x14,0x14,0x18,0x7C}, // 113 q
      {0x7C,0x08,0x04,0x04,0x08}, // 114 r
      {0x48,0x54,0x54,0x54,0x20}, // 115 s
      {0x04,0x3F,0x44,0x40,0x20}, // 116 t
      {0x3C,0x40,0x40,0x20,0x7C}, // 117 u
      {0x1C,0x20,0x40,0x20,0x1C}, // 118 v
      {0x3C,0x40,0x30,0x40,0x3C}, // 119 w
      {0x44,0x28,0x10,0x28,0x44}, // 120 x
      {0x0C,0x50,0x50,0x50,0x3C}, // 121 y
      {0x44,0x64,0x54,0x4C,0x44}, // 122 z
      {0x00,0x08,0x36,0x41,0x00}, // 123 {
      {0x00,0x00,0x7F,0x00,0x00}, // 124 |
      {0x00,0x41,0x36,0x08,0x00}, // 125 }
      {0x08,0x08,0x2A,0x1C,0x08}, // 126 ~
    };
    int idx = (int)c - 32;
    if (idx < 0 || idx >= 95) return false;
    return (font[idx][x] >> y) & 1;
  }

 public:
  GfxRenderer() { clearScreen(); }

  int getScreenWidth() const { return SW; }
  int getScreenHeight() const { return SH; }

  void clearScreen(uint8_t = 0) { memset(fb, 0, sizeof(fb)); }

  void displayBuffer(int = 0) {} // no-op in preview

  void fillRect(int x, int y, int w, int h, bool black = true) {
    for (int py = y; py < y + h; py++)
      for (int px = x; px < x + w; px++)
        setPixel(px, py, black);
  }

  void drawRect(int x, int y, int w, int h, bool = true) {
    for (int i = 0; i < w; i++) { setPixel(x + i, y, true); setPixel(x + i, y + h - 1, true); }
    for (int i = 0; i < h; i++) { setPixel(x, y + i, true); setPixel(x + w - 1, y + i, true); }
  }

  void drawLine(int x1, int y1, int x2, int y2) {
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    while (true) {
      setPixel(x1, y1, true);
      if (x1 == x2 && y1 == y2) break;
      int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x1 += sx; }
      if (e2 <= dx) { err += dx; y1 += sy; }
    }
  }

  void drawPixel(int x, int y, bool black = true) { setPixel(x, y, black); }

  void drawText(int font, int x, int y, const char* text, bool black = true, int style = 0) {
    int scale = (font >= 12) ? 3 : (font >= 10) ? 2 : 2;
    if (style == EpdFontFamily::BOLD) {
      // Bold: draw twice offset by 1
      for (const char* p = text; *p; p++) {
        drawChar(x, y, *p, black, scale);
        drawChar(x + 1, y, *p, black, scale);
        x += (5 * scale + scale);
      }
    } else {
      for (const char* p = text; *p; p++) {
        drawChar(x, y, *p, black, scale);
        x += (5 * scale + scale);
      }
    }
  }

  void drawCenteredText(int font, int y, const char* text, bool black = true, int style = 0) {
    int tw = getTextWidth(font, text, style);
    drawText(font, (SW - tw) / 2, y, text, black, style);
  }

  int getTextWidth(int font, const char* text, int = 0) {
    int scale = (font >= 12) ? 3 : 2;
    int len = (int)strlen(text);
    return len * (5 * scale + scale);
  }

  int getTextHeight(int font) {
    int scale = (font >= 12) ? 3 : 2;
    return 7 * scale;
  }

  int getLineHeight(int font) { return getTextHeight(font) + 4; }

  std::string truncatedText(int, const char* t, int, int = 0) { return t; }
  std::vector<std::string> wrappedText(int, const char* t, int, int = 10) { return {t}; }

  void invertScreen() {
    for (int i = 0; i < SW * SH; i++) fb[i] = fb[i] ? 0 : 1;
  }
  void setOrientation(Orientation) {}
  Orientation getOrientation() const { return Portrait; }
  uint8_t* getFrameBuffer() { return fb; }
  static size_t getBufferSize() { return SW * SH; }

  // ============================================================
  // Save framebuffer as BMP (480x800 1-bit monochrome)
  // ============================================================
  bool saveBMP(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;

    int rowBytes = ((SW + 31) / 32) * 4;  // BMP rows padded to 4 bytes
    int imageSize = rowBytes * SH;
    int fileSize = 62 + imageSize;

    // File header (14 bytes)
    uint8_t header[62] = {};
    header[0] = 'B'; header[1] = 'M';
    header[2] = fileSize & 0xFF; header[3] = (fileSize >> 8) & 0xFF;
    header[4] = (fileSize >> 16) & 0xFF; header[5] = (fileSize >> 24) & 0xFF;
    header[10] = 62;  // pixel data offset

    // Info header (40 bytes)
    header[14] = 40;
    header[18] = SW & 0xFF; header[19] = (SW >> 8) & 0xFF;
    header[22] = SH & 0xFF; header[23] = (SH >> 8) & 0xFF;
    header[26] = 1;   // planes
    header[28] = 1;   // bits per pixel

    header[34] = imageSize & 0xFF; header[35] = (imageSize >> 8) & 0xFF;
    header[36] = (imageSize >> 16) & 0xFF; header[37] = (imageSize >> 24) & 0xFF;

    // Palette: index 0 = white (#F5F0E8 warm), index 1 = black
    header[54] = 0xE8; header[55] = 0xF0; header[56] = 0xF5; header[57] = 0x00;
    // index 1 = black (already zeros)

    fwrite(header, 1, 62, f);

    // Write pixel data (BMP is bottom-up)
    uint8_t* row = new uint8_t[rowBytes];
    for (int y = SH - 1; y >= 0; y--) {
      memset(row, 0, rowBytes);
      for (int x = 0; x < SW; x++) {
        if (fb[y * SW + x]) {
          row[x / 8] |= (0x80 >> (x % 8));
        }
      }
      fwrite(row, 1, rowBytes, f);
    }
    delete[] row;

    fclose(f);
    return true;
  }
};
