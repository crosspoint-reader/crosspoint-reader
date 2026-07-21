#include "SimulatorControls.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace SimulatorControls {
namespace {

constexpr int OUTER_MARGIN = 16;
constexpr int PANEL_GAP = 16;
constexpr int SIDEBAR_WIDTH = 176;
constexpr int MIN_WINDOW_HEIGHT = 512;

std::mutex stateMutex;
Layout currentLayout;
std::array<bool, BUTTON_COUNT> keyboardDown{};
std::array<bool, BUTTON_COUNT> mouseDown{};
int hoveredButton = -1;
std::atomic<bool> redrawRequested{true};
std::atomic<bool> screenshotRequested{false};

const char *buttonLabels[BUTTON_COUNT] = {
    "BACK", "CONFIRM", "LEFT", "RIGHT", "UP", "DOWN", "POWER",
};
const char *buttonShortcuts[BUTTON_COUNT] = {
    "ESC", "ENTER", "LEFT KEY", "RIGHT KEY", "UP KEY", "DOWN KEY", "P",
};

bool contains(const SDL_Rect &rect, int x, int y) {
  return x >= rect.x && y >= rect.y && x < rect.x + rect.w &&
         y < rect.y + rect.h;
}

std::array<uint8_t, 7> glyph(char value) {
  switch (value) {
  case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
  case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
  case 'C': return {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
  case 'D': return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
  case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
  case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
  case 'G': return {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E};
  case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
  case 'I': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
  case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
  case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
  case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
  case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
  case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
  case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
  case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
  case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
  case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
  case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
  case 'V': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
  case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
  case 'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
  case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
  case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
  case '1': return {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
  case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
  case '3': return {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
  case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
  case '5': return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
  case '6': return {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
  case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
  case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
  case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
  case '-': return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
  case ':': return {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
  default: return {};
  }
}

void setColor(SDL_Renderer *renderer, uint32_t rgb) {
  SDL_SetRenderDrawColor(renderer, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff,
                         rgb & 0xff, 0xff);
}

void drawText(SDL_Renderer *renderer, int x, int y, const char *text,
              int scale, uint32_t color) {
  setColor(renderer, color);
  const int advance = 6 * scale;
  for (const char *cursor = text; cursor && *cursor; ++cursor, x += advance) {
    if (*cursor == ' ') continue;
    const auto rows = glyph(*cursor);
    for (int row = 0; row < 7; ++row) {
      for (int column = 0; column < 5; ++column) {
        if ((rows[row] & (1u << (4 - column))) == 0) continue;
        SDL_Rect pixel{x + column * scale, y + row * scale, scale, scale};
        SDL_RenderFillRect(renderer, &pixel);
      }
    }
  }
}

} // namespace

Layout makeLayout(int screenWidth, int screenHeight) {
  Layout layout;
  layout.windowWidth = OUTER_MARGIN + screenWidth + PANEL_GAP + SIDEBAR_WIDTH + OUTER_MARGIN;
  layout.windowHeight = std::max(screenHeight + OUTER_MARGIN * 2, MIN_WINDOW_HEIGHT);
  layout.screen = {OUTER_MARGIN, (layout.windowHeight - screenHeight) / 2,
                   screenWidth, screenHeight};
  layout.sidebar = {OUTER_MARGIN + screenWidth + PANEL_GAP, OUTER_MARGIN,
                    SIDEBAR_WIDTH, layout.windowHeight - OUTER_MARGIN * 2};

  constexpr int buttonHeight = 44;
  constexpr int buttonGap = 8;
  const int buttonX = layout.sidebar.x + 12;
  const int buttonWidth = layout.sidebar.w - 24;
  const int startY = layout.sidebar.y + 50;
  for (int index = 0; index < BUTTON_COUNT; ++index) {
    layout.buttons[index] = {buttonX, startY + index * (buttonHeight + buttonGap),
                             buttonWidth, buttonHeight};
  }
  return layout;
}

void setLayout(const Layout &layout) {
  std::lock_guard<std::mutex> lock(stateMutex);
  currentLayout = layout;
}

Layout getLayout() {
  std::lock_guard<std::mutex> lock(stateMutex);
  return currentLayout;
}

int hitTest(int x, int y) {
  std::lock_guard<std::mutex> lock(stateMutex);
  for (int index = 0; index < BUTTON_COUNT; ++index) {
    if (contains(currentLayout.buttons[index], x, y)) return index;
  }
  return -1;
}

void setKeyboardDown(int button, bool down) {
  if (button < 0 || button >= BUTTON_COUNT) return;
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    keyboardDown[button] = down;
  }
  requestRedraw();
}

void setMouseDown(int button, bool down) {
  if (button < 0 || button >= BUTTON_COUNT) return;
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    mouseDown[button] = down;
  }
  requestRedraw();
}

bool isMouseDown(int button) {
  if (button < 0 || button >= BUTTON_COUNT) return false;
  std::lock_guard<std::mutex> lock(stateMutex);
  return mouseDown[button];
}

bool isVisuallyDown(int button) {
  if (button < 0 || button >= BUTTON_COUNT) return false;
  std::lock_guard<std::mutex> lock(stateMutex);
  return keyboardDown[button] || mouseDown[button];
}

void releaseMouseButtons() {
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    mouseDown.fill(false);
  }
  requestRedraw();
}

void setHoveredButton(int button) {
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (hoveredButton != button) {
      hoveredButton = button;
      changed = true;
    }
  }
  if (changed) requestRedraw();
}

void requestRedraw() { redrawRequested.store(true); }
bool takeRedrawRequest() { return redrawRequested.exchange(false); }
void requestScreenshot() {
  screenshotRequested.store(true);
  requestRedraw();
}
bool takeScreenshotRequest() { return screenshotRequested.exchange(false); }

void draw(SDL_Renderer *renderer, const Layout &layout, const char *deviceName) {
  if (!renderer) return;

  int hover = -1;
  std::array<bool, BUTTON_COUNT> down{};
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    hover = hoveredButton;
    for (int index = 0; index < BUTTON_COUNT; ++index) {
      down[index] = keyboardDown[index] || mouseDown[index];
    }
  }

  setColor(renderer, 0x20252B);
  SDL_RenderFillRect(renderer, &layout.sidebar);
  setColor(renderer, 0x56606A);
  SDL_RenderDrawRect(renderer, &layout.sidebar);

  drawText(renderer, layout.sidebar.x + 12, layout.sidebar.y + 12,
           "CROSSVI", 2, 0xF0F1EC);
  drawText(renderer, layout.sidebar.x + 110, layout.sidebar.y + 18,
           deviceName, 1, 0xAEB6BE);

  for (int index = 0; index < BUTTON_COUNT; ++index) {
    const SDL_Rect &button = layout.buttons[index];
    const uint32_t fill = down[index] ? 0xF0F1EC : (hover == index ? 0x46515C : 0x303841);
    const uint32_t text = down[index] ? 0x15181C : 0xF0F1EC;
    setColor(renderer, fill);
    SDL_RenderFillRect(renderer, &button);
    setColor(renderer, hover == index ? 0xD0D5D9 : 0x68737E);
    SDL_RenderDrawRect(renderer, &button);
    drawText(renderer, button.x + 9, button.y + 6, buttonLabels[index], 2, text);
    drawText(renderer, button.x + 10, button.y + 28, buttonShortcuts[index], 1,
             down[index] ? 0x394047 : 0xAEB6BE);
  }

  drawText(renderer, layout.sidebar.x + 12,
           layout.sidebar.y + layout.sidebar.h - 20, "F12 SCREENSHOT", 1,
           0x8F99A3);
}

} // namespace SimulatorControls
