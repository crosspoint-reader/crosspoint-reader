#pragma once

#include <cstdint>

#include "RemindersState.h"

class GfxRenderer;

/**
 * RemindersRenderer — draws the Taskpoint countdown task list on the 480x800
 * 1-bit e-ink panel. Orientation-agnostic: it queries the renderer for the
 * logical screen size rather than assuming 480/800, and forces Portrait.
 *
 * The list is paginated: a page starts at `startIndex` and fills the available
 * height; the methods return the index of the first item that did not fit
 * (== count on the last page) so the caller can drive Up/Down paging.
 *
 * Both entry points build the full frame into the framebuffer; they differ only
 * in the refresh waveform:
 *   - renderFull():           HALF_REFRESH (clean) for first paint / paging / re-sync
 *   - renderCountdownsOnly():  FAST_REFRESH (flash-free) for the per-minute tick
 *
 * The stale "NOT LIVE" banner is drawn automatically when data.is_stale is set.
 */
class RemindersRenderer {
 public:
  // Draw the page starting at startIndex; returns the next page's start index
  // (== data.count when this was the last page). Presents with HALF_REFRESH.
  static uint8_t renderFull(GfxRenderer& renderer, const RemindersData& data, uint8_t startIndex = 0);

  // Re-render the same page (countdowns recomputed) with FAST_REFRESH. Returns
  // false if a timed item has elapsed since the last sync, signalling the caller
  // to fall back to a full refresh to clear ghosting.
  static bool renderCountdownsOnly(GfxRenderer& renderer, const RemindersData& data, uint8_t startIndex = 0);

 private:
  // Render one page into the framebuffer (no present). Returns the next page's
  // start index.
  static uint8_t drawLayout(GfxRenderer& renderer, const RemindersData& data, uint8_t startIndex);
};
