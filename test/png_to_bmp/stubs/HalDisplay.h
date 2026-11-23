#pragma once

// Cover sizing swaps the panel dimensions; match the 129x2 PNG fixture.
struct PngTestDisplay {
  int getDisplayWidth() const { return 2; }
  int getDisplayHeight() const { return 129; }
};
inline PngTestDisplay display;
