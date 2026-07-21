#pragma once

#include <SDL.h>

#include <array>

namespace SimulatorControls {

constexpr int BUTTON_COUNT = 7;

struct Layout {
  int windowWidth = 0;
  int windowHeight = 0;
  SDL_Rect screen{};
  SDL_Rect sidebar{};
  std::array<SDL_Rect, BUTTON_COUNT> buttons{};
};

Layout makeLayout(int screenWidth, int screenHeight);
void setLayout(const Layout &layout);
Layout getLayout();
int hitTest(int x, int y);

void setKeyboardDown(int button, bool down);
void setMouseDown(int button, bool down);
bool isMouseDown(int button);
bool isVisuallyDown(int button);
void releaseMouseButtons();
void setHoveredButton(int button);

void requestRedraw();
bool takeRedrawRequest();
void requestScreenshot();
bool takeScreenshotRequest();

void draw(SDL_Renderer *renderer, const Layout &layout, const char *deviceName);

} // namespace SimulatorControls
