#pragma once
#if CROSSPOINT_DESSEN_APPS

#include <FreeInkUIGfxRenderer.h>
#include <freeink/DessenFreeInk.h>

#include <memory>
#include <string>

#include "CrosspointServices.h"
#include "activities/Activity.h"

// Runs one dessen app (a sandboxed RV32IM binary, e.g. <plugin dir>/app.bin)
// full-screen. The app draws through the same FreeInkUI target as the
// firmware's own screens and receives confirm/back/prev/next/tap events.
// Holding Back for a second always leaves, so an app cannot trap the user.
class DessenAppActivity final : public Activity {
 public:
  DessenAppActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string appPath);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Every app gets one fixed block; nothing else is allocated while it runs.
  static constexpr uint32_t APP_MEMORY_BYTES = 64 * 1024;

  bool loadApp();
  void openKeyboard();  // service asked for text input

  std::string appPath;
  freeink::ui::GfxRendererTarget uiTarget;
  std::unique_ptr<uint8_t[]> memory;
  // host, pendingInput: written by the render task and by loop() under RenderLock.
  dessen::FreeInkHost host;
  freeink::ui::InputSnapshot pendingInput{};
  std::unique_ptr<CrosspointServices> services;
  bool keyboardOpen = false;
};

#endif  // CROSSPOINT_DESSEN_APPS
