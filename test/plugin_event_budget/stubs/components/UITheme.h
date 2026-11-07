#pragma once
class GfxRenderer {};
struct FixtureGUI {
  void drawPopup(GfxRenderer&, const char*) {}
};
inline FixtureGUI GUI;
