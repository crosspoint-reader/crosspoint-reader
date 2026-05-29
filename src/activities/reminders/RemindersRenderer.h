#pragma once

#include "RemindersState.h"

class GfxRenderer;

/**
 * RemindersRenderer — draws the Taskpoint countdown task list on the 480x800
 * 1-bit e-ink panel. Orientation-agnostic: it queries the renderer for the
 * logical screen size rather than assuming 480/800.
 *
 * All three entry points build the full frame into the framebuffer; they differ
 * only in the refresh waveform used to push it to the panel:
 *   - renderFull():           HALF_REFRESH (clean, ~1.7s) for first paint / re-sync
 *   - renderCountdownsOnly():  FAST_REFRESH (flash-free) for the per-minute tick
 *   - renderStaleBanner():     overlays the "NOT LIVE" banner, then HALF_REFRESH
 */
class RemindersRenderer {
 public:
  // Draw the whole layout and present it with HALF_REFRESH.
  static void renderFull(GfxRenderer& renderer, const RemindersData& data);

  // Rebuild the layout (countdowns recomputed against the current clock) and
  // present it with FAST_REFRESH. Returns false if an item's start time has
  // passed since synced_epoch, signalling the caller to fall back to a full
  // refresh to clear any ghosting from the elapsed item.
  static bool renderCountdownsOnly(GfxRenderer& renderer, const RemindersData& data);

  // Draw the layout, overlay the stale banner, present with HALF_REFRESH.
  static void renderStaleBanner(GfxRenderer& renderer, const RemindersData& data);

 private:
  // Render the entire frame into the framebuffer without presenting it.
  static void drawLayout(GfxRenderer& renderer, const RemindersData& data);
  static void drawStaleOverlay(GfxRenderer& renderer, const RemindersData& data);
};
