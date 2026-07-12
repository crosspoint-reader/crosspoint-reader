#pragma once

class GfxRenderer;

struct RadarNode {
  const char* label;
  int appCount;
};

struct RadarHomeStatus {
  const char* radioLeft;    // e.g. "wifi:OFF  ble:OFF"
  const char* systemRight;  // e.g. "heap:142K"
  int batteryPercent;
};

class RadarHomeRenderer {
 public:
  static constexpr int CENTER_X = 240;
  static constexpr int CENTER_Y = 290;
  static constexpr int RADIUS_OUTER = 190;
  static constexpr int RADIUS_MID = 130;
  static constexpr int RADIUS_INNER = 70;
  static constexpr int NODE_RING_RADIUS = 185;
  static constexpr int NODE_COUNT = 8;
  static constexpr int NODE_W = 80;
  static constexpr int NODE_H = 28;
  static constexpr int NODE_CORNER = 4;

  static void draw(GfxRenderer& renderer, const RadarNode* nodes, int selectedIndex,
                   const RadarHomeStatus& status);

 private:
  static void drawCircle(GfxRenderer& r, int cx, int cy, int radius);
  static void drawDashedLine(GfxRenderer& r, int x1, int y1, int x2, int y2, int dashLen, int gapLen);
  static void drawRoundRectOutline(GfxRenderer& r, int x, int y, int w, int h, int radius);
  static void fillRoundRect(GfxRenderer& r, int x, int y, int w, int h, int radius);
  static void drawTextCenteredInRect(GfxRenderer& r, int rx, int ry, int rw, int rh, const char* text,
                                     bool blackText);
};
