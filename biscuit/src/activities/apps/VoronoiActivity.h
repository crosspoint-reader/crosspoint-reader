#pragma once
#include <cstdint>

#include "activities/Activity.h"

class VoronoiActivity final : public Activity {
 public:
  explicit VoronoiActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Voronoi", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int MAX_POINTS = 40;
  int numPoints = 20;

  struct Point {
    int x, y;
  };
  Point points[MAX_POINTS]{};
  int genWidth = 480;
  int genHeight = 800;

  void generate();

  static constexpr int GRID_STEP = 8;
  static constexpr int MAX_GRID_W = 100;  // 800/8
  static constexpr int MAX_GRID_H = 60;   // 480/8
  uint8_t nearestGrid[MAX_GRID_H][MAX_GRID_W]{};
  int gridW = 0, gridH = 0;

  void precompute();
};
