#pragma once

#include <FreeInkUI.h>

#include <cstring>

// Minimal DrawTarget fake for geometry tests: no pixels, just the measuring
// the FreeInkUI row components need (fixed metric font; text width = 6px per
// byte). Mirrors the SDK's own host-test fake.
class GeometryTarget final : public freeink::ui::DrawTarget {
 public:
  freeink::ui::Size measureText(freeink::ui::FontId, const char* text, freeink::ui::TextStyle) const override {
    return freeink::ui::Size{static_cast<int16_t>(text && *text ? 6 * static_cast<int>(std::strlen(text)) : 0), 24};
  }
  int16_t lineHeight(freeink::ui::FontId) const override { return 24; }
  void fill(freeink::ui::Rect, freeink::ui::Paint, uint8_t = 0, uint8_t = freeink::ui::CornersAll) override {}
  void stroke(freeink::ui::Rect, freeink::ui::Paint, uint8_t, uint8_t = 0, uint8_t = freeink::ui::CornersAll) override {
  }
  void line(freeink::ui::Point, freeink::ui::Point, uint8_t, freeink::ui::Paint) override {}
  void triangle(freeink::ui::Point, freeink::ui::Point, freeink::ui::Point, freeink::ui::Paint) override {}
  void text(freeink::ui::Rect, const char*, freeink::ui::TextStyle) override {}
  void bitmap(freeink::ui::Rect, freeink::ui::BitmapRef, freeink::ui::BitmapMode,
              freeink::ui::Paint = freeink::ui::Paint::solid(freeink::ui::Color::Black),
              freeink::ui::Rotation = freeink::ui::Rotation::None) override {}
};

inline freeink::ui::DeviceContext geometryDevice(const bool touchCapable, const int16_t minTouchSize) {
  freeink::ui::DeviceContext device{};
  device.width = 480;
  device.height = 800;
  device.hasTouch = touchCapable;
  device.hasButtons = false;
  device.minTouchSize = minTouchSize;
  return device;
}

inline freeink::ui::InputSnapshot tapAt(const int16_t x, const int16_t y) {
  freeink::ui::InputSnapshot snap{};
  snap.touchReleased = true;
  snap.touchX = x;
  snap.touchY = y;
  return snap;
}
