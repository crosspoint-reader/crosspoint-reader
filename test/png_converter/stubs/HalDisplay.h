#pragma once
struct TestDisplay {
  int getDisplayWidth() const { return 32; }
  int getDisplayHeight() const { return 32; }
};
inline TestDisplay display;
