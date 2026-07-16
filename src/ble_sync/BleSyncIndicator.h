// BleSyncIndicator — the small top-left sync glyph + label.
//
// Draws into the current activity's header: a rotating arc while syncing, a
// ✓-in-circle on success, a ✗-in-circle on failure, plus a short status label
// ("Sync 1/3", "Synced", "Sync failed"). Reads BLE_SYNC status; draws nothing
// when idle with no fresh result. Cheap — a few arcs/lines + one small string.
#pragma once

class GfxRenderer;

namespace BleSync {

// Draw the indicator into the given header rect, aligned to the battery band
// (same vertical center + glyph size + SMALL font as the battery, anchored to the
// header's LEFT). No-op when there's nothing to show. Returns true if it drew.
bool drawIndicator(GfxRenderer& renderer, int headerX, int headerY, int headerW, int headerH);

// True when the indicator wants the screen on-screen (active sync OR a fresh
// ✓/✗ still lingering) — lets an activity decide whether to include it.
bool indicatorVisible();

}  // namespace BleSync
