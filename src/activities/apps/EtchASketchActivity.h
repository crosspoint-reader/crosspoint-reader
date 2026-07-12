#pragma once

#include <cstdint>
#include <string>

#include "activities/Activity.h"

class EtchASketchActivity final : public Activity {
 public:
  explicit EtchASketchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("EtchASketch", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  int cursorX = 0;
  int cursorY = 0;
  bool penDown = true;

  // Canvas stored as bitmap
  static constexpr int CANVAS_MAX_W = 480;
  static constexpr int CANVAS_MAX_H = 800;
  // Packed bit array: 1 bit per pixel
  static constexpr int CANVAS_BYTES = (CANVAS_MAX_W * CANVAS_MAX_H + 7) / 8;
  uint8_t* canvas = nullptr;

  int canvasW = 0;
  int canvasH = 0;

  void setPixel(int x, int y);
  bool getPixel(int x, int y) const;
  void clearCanvas();
  void saveToBmp();
};
