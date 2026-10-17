#pragma once
// Adapted from CrossPlay (MIT); see ../LICENSE.crossplay.

#include <cstddef>
#include <cstdint>

constexpr int SDL_SCANCODE_PAGEUP = 75;
constexpr int SDL_SCANCODE_PAGEDOWN = 78;

extern "C" {

// -- init / teardown ---------------------------------------------------------
constexpr uint32_t SDL_INIT_VIDEO = 0x20u;
int SDL_Init(uint32_t flags);
void SDL_Quit();
const char* SDL_GetError();

// -- opaque handles ----------------------------------------------------------
// Never dereferenced by the simulator; it only null-checks them.
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Surface;

constexpr int SDL_WINDOWPOS_UNDEFINED = 0x1FFF0000;
constexpr uint32_t SDL_WINDOW_SHOWN = 0x00000004u;
constexpr uint32_t SDL_WINDOW_ALLOW_HIGHDPI = 0x00002000u;
constexpr uint32_t SDL_RENDERER_ACCELERATED = 0x00000002u;
constexpr int SDL_TEXTUREACCESS_STREAMING = 1;
constexpr uint32_t SDL_PIXELFORMAT_ARGB8888 = 0x16362004u;

#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_RENDER_SCALE_QUALITY"

SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, uint32_t flags);
SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, uint32_t flags);
SDL_Texture* SDL_CreateTexture(SDL_Renderer* r, uint32_t format, int access, int w, int h);
int SDL_SetHint(const char* name, const char* value);
void SDL_SetWindowSize(SDL_Window* window, int w, int h);
int SDL_RenderSetLogicalSize(SDL_Renderer* r, int w, int h);
int SDL_GetRendererOutputSize(SDL_Renderer* r, int* w, int* h);

// -- presenting --------------------------------------------------------------
struct SDL_Rect {
  int x, y, w, h;
};
struct SDL_Point {
  int x, y;
};
enum SDL_RendererFlip { SDL_FLIP_NONE = 0 };

int SDL_UpdateTexture(SDL_Texture* t, const SDL_Rect* rect, const void* pixels, int pitch);
int SDL_RenderClear(SDL_Renderer* r);
int SDL_RenderCopy(SDL_Renderer* r, SDL_Texture* t, const SDL_Rect* src, const SDL_Rect* dst);
int SDL_RenderCopyEx(SDL_Renderer* r, SDL_Texture* t, const SDL_Rect* src, const SDL_Rect* dst, double angle,
                     const SDL_Point* center, SDL_RendererFlip flip);
void SDL_RenderPresent(SDL_Renderer* r);

// Screenshots are a desktop-harness feature (scripts_local/sim-shot.sh). The
// browser has no file to write, so these fail politely rather than pretending.
int SDL_RenderReadPixels(SDL_Renderer* r, const SDL_Rect* rect, uint32_t format, void* pixels, int pitch);
SDL_Surface* SDL_CreateRGBSurfaceWithFormatFrom(void* pixels, int w, int h, int depth, int pitch, uint32_t format);
int SDL_SaveBMP(SDL_Surface* surface, const char* file);
void SDL_FreeSurface(SDL_Surface* surface);

// -- time --------------------------------------------------------------------
uint32_t SDL_GetTicks();
void SDL_Delay(uint32_t ms);

// -- input -------------------------------------------------------------------
typedef int SDL_Scancode;
constexpr SDL_Scancode SDL_SCANCODE_UNKNOWN = 0;
constexpr SDL_Scancode SDL_SCANCODE_H = 11;
constexpr SDL_Scancode SDL_SCANCODE_P = 19;
constexpr SDL_Scancode SDL_SCANCODE_S = 22;
constexpr SDL_Scancode SDL_SCANCODE_RETURN = 40;
constexpr SDL_Scancode SDL_SCANCODE_ESCAPE = 41;
constexpr SDL_Scancode SDL_SCANCODE_RIGHT = 79;
constexpr SDL_Scancode SDL_SCANCODE_LEFT = 80;
constexpr SDL_Scancode SDL_SCANCODE_DOWN = 81;
constexpr SDL_Scancode SDL_SCANCODE_UP = 82;
constexpr int SDL_NUM_SCANCODES = 512;

constexpr uint32_t SDL_QUIT = 0x100;
constexpr uint32_t SDL_KEYDOWN = 0x300;
constexpr uint32_t SDL_KEYUP = 0x301;
constexpr uint32_t SDL_MOUSEMOTION = 0x400;
constexpr uint32_t SDL_MOUSEBUTTONDOWN = 0x401;
constexpr uint32_t SDL_MOUSEBUTTONUP = 0x402;
constexpr uint8_t SDL_BUTTON_LEFT = 1;

struct SDL_Keysym {
  SDL_Scancode scancode;
};
struct SDL_KeyboardEvent {
  uint32_t type;
  uint8_t repeat;
  SDL_Keysym keysym;
};
struct SDL_MouseButtonEvent {
  uint32_t type;
  uint8_t button;
  int32_t x, y;
};
struct SDL_MouseMotionEvent {
  uint32_t type;
  int32_t x, y;
};

union SDL_Event {
  uint32_t type;
  SDL_KeyboardEvent key;
  SDL_MouseButtonEvent button;
  SDL_MouseMotionEvent motion;
};

int SDL_PollEvent(SDL_Event* event);
const uint8_t* SDL_GetKeyboardState(int* numkeys);

}  // extern "C"
