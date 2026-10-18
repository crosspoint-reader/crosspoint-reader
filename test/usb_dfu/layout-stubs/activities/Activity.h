#pragma once
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "Arduino.h"
struct Rect {
  int x, y, width, height;
};
struct EpdFontFamily {
  enum Style { REGULAR, BOLD };
};
class GfxRenderer {
 public:
  struct Op {
    Rect rect;
    std::string text;
    bool fill;
  };
  int width = 600, height = 800, line = 24;
  std::vector<Op> ops;
  bool pending = false;
  unsigned asyncPaints = 0, blockingPaints = 0;
  int getScreenWidth() const { return width; }
  int getScreenHeight() const { return height; }
  int getLineHeight(int) const { return line; }
  int getTextHeight(int) const { return line; }
  void getOrientedViewableTRBL(int* t, int* r, int* b, int* l) const { *t = *r = *b = *l = 10; }
  void clearScreen() {
    assert(!pending);
    ops.clear();
  }
  void drawCenteredText(int, int y, const char* text, bool = true, EpdFontFamily::Style = EpdFontFamily::REGULAR) {
    int w = int(std::string(text).size()) * line / 2;
    ops.push_back({{(width - w) / 2, y, w, line}, text, false});
  }
  void drawRect(int x, int y, int w, int h) { ops.push_back({{x, y, w, h}, "", false}); }
  void fillRect(int x, int y, int w, int h) { ops.push_back({{x, y, w, h}, "", true}); }
  void displayBuffer() {
    assert(!pending);
    ++blockingPaints;
  }
  void displayBufferAsync() {
    assert(!pending);
    pending = true;
    ++asyncPaints;
  }
  void waitRefreshComplete() { pending = false; }
};
struct MappedInputManager {};
struct RenderLock {};
struct Manager {
  void goHome() {}
};
inline Manager activityManager;
class Activity {
 protected:
  GfxRenderer& renderer;

 public:
  Activity(const char*, GfxRenderer& r, MappedInputManager&) : renderer(r) {}
  virtual ~Activity() = default;
  virtual void onEnter() {}
  virtual void onExit() {}
  virtual void loop() {}
  virtual void render(RenderLock&&) {}
  virtual bool requiresExclusiveStorageLoop() const { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual bool skipLoopDelay() { return false; }
  void requestUpdateAndWait() {}
};
