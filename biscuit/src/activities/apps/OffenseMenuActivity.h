#pragma once
#include "activities/Activity.h"

class OffenseMenuActivity final : public Activity {
 public:
  explicit OffenseMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OffenseMenu", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  int selectorIndex = 0;
  static constexpr int ITEM_COUNT = 4;
  static constexpr int COLS = 2;
  static constexpr int ROWS = 2;
  bool disclaimerShown = false;

  int getRow() const { return selectorIndex / COLS; }
  int getCol() const { return selectorIndex % COLS; }

  void drawTile(int index, int x, int y, int w, int h, bool selected) const;
  void openSubTile(int index);
};
