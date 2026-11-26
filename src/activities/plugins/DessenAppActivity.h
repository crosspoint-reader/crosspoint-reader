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
  DessenAppActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string appPath,
                    bool homeLauncher = false);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;

 private:
  // Every app gets one fixed block; nothing else is allocated while it runs.
  // The app declares how much memory it wants in its image header; we allocate
  // exactly that (one block), capped for sanity. On PSRAM boards a block over
  // ~4 KB lands in PSRAM automatically, so apps can ask for hundreds of KB.
#ifdef BOARD_HAS_PSRAM
  static constexpr uint32_t MAX_APP_MEMORY = 4u * 1024 * 1024;
#else
  static constexpr uint32_t MAX_APP_MEMORY = 128u * 1024;  // C3: internal RAM only
#endif

  bool loadApp();
  void leaveApp();
  void openKeyboard();       // service asked for text input
  bool storeOverlayFrame();  // best effort, under RenderLock

  std::string appPath;
  const bool homeLauncher;
  freeink::ui::GfxRendererTarget uiTarget;
  std::unique_ptr<uint8_t[]> memory;
  // host, pendingInput: written by the render task and by loop() under RenderLock.
  dessen::FreeInkHost host;
  freeink::ui::InputSnapshot pendingInput{};
  dessen::FreeInkHost::Swipe pendingSwipe = dessen::FreeInkHost::SwipeNone;
  std::unique_ptr<CrosspointServices> services;
  // The app's last drawn frame is snapshotted so it can be restored when a
  // system overlay (e.g. the frontlight panel) pops: the app is parked in
  // wait_event and cannot repaint itself, so the overlay would otherwise linger
  // until the app next draws.
  bool frameStored = false;
};

#endif  // CROSSPOINT_DESSEN_APPS
