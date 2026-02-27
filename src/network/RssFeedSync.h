#pragma once

namespace RssFeedSync {

/// Start background sync task. No-op if feed URL empty, WiFi not connected, or already running.
void startSync();

/// Returns true if a sync task is currently running.
bool isSyncing();

}  // namespace RssFeedSync
