#pragma once

#include <cstdlib>
#include <string>

namespace ThemeEngine {

enum class DimensionUnit { PIXELS, PERCENT, UNKNOWN };

struct Dimension {
  int value;
  DimensionUnit unit;

  Dimension(int v, DimensionUnit u) : value(v), unit(u) {}
  Dimension() : value(0), unit(DimensionUnit::PIXELS) {}

  static Dimension parse(const std::string &str) {
    if (str.empty())
      return Dimension(0, DimensionUnit::PIXELS);

    if (str.back() == '%') {
      return Dimension(std::stoi(str.substr(0, str.length() - 1)),
                       DimensionUnit::PERCENT);
    }
    return Dimension(std::stoi(str), DimensionUnit::PIXELS);
  }

  int resolve(int parentSize) const {
    if (unit == DimensionUnit::PERCENT) {
      return (parentSize * value) / 100;
    }
    return value;
  }
};

struct Color {
  uint8_t value; // For E-Ink: 0 (Black) to 255 (White), or simplified palette

  Color(uint8_t v) : value(v) {}
  Color() : value(0) {}

  static Color parse(const std::string &str) {
    if (str.empty())
      return Color(0);
    if (str == "black")
      return Color(0x00);
    if (str == "white")
      return Color(0xFF);
    if (str == "gray" || str == "grey")
      return Color(0x80);
    if (str.size() > 2 && str.substr(0, 2) == "0x") {
      return Color((uint8_t)std::strtol(str.c_str(), nullptr, 16));
    }
    return Color((uint8_t)std::stoi(str));
  }
};

// Rect structure for dirty regions
struct Rect {
  int x, y, w, h;

  Rect() : x(0), y(0), w(0), h(0) {}
  Rect(int x, int y, int w, int h) : x(x), y(y), w(w), h(h) {}

  bool isEmpty() const { return w <= 0 || h <= 0; }

  bool intersects(const Rect &other) const {
    return !(x + w <= other.x || other.x + other.w <= x || y + h <= other.y ||
             other.y + other.h <= y);
  }

  Rect unite(const Rect &other) const {
    if (isEmpty())
      return other;
    if (other.isEmpty())
      return *this;
    int nx = std::min(x, other.x);
    int ny = std::min(y, other.y);
    int nx2 = std::max(x + w, other.x + other.w);
    int ny2 = std::max(y + h, other.y + other.h);
    return Rect(nx, ny, nx2 - nx, ny2 - ny);
  }
};

} // namespace ThemeEngine
