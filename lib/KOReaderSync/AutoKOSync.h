#pragma once
#include <cstdint>
#include <string>

#include "KOReaderDocumentId.h"
#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"

/**
 * Automatic KOReader sync on book open/close.
 *
 * Direction-aware: only pushes when local is ahead, only pulls when remote is ahead.
 * Battery-optimized: skip rules prevent unnecessary WiFi activations.
 * Self-contained: no UI, no Activity dependencies.
 */
class AutoKOSync {
 public:
  // Setting values (stored as uint8_t in CrossPointSettings)
  static constexpr uint8_t OFF = 0;
  static constexpr uint8_t ON_CLOSE = 1;
  static constexpr uint8_t ON_OPEN_CLOSE = 2;

  /** Result of a sync operation for UI feedback. */
  enum class SyncStatus : uint8_t {
    SUCCESS,      // Sync completed (uploaded, or remote same/ahead — no action needed)
    SKIPPED,      // Skip rule fired (cooldown, unchanged, etc.)
    WIFI_FAILED,  // Could not connect to WiFi
    SYNC_FAILED,  // WiFi OK but HTTP GET/PUT failed
  };

  /**
   * Precomputed sync payload — built while Epub is still in memory,
   * executed after Epub is released (for heap headroom).
   */
  struct ClosePayload {
    std::string documentHash;
    std::string epubPath;
    KOReaderPosition localKoPos;
    float localPercentage;
    std::string wifiSsid;
    std::string wifiPassword;
    bool valid = false;
  };

  /**
   * Result of sync-on-open fetch.
   */
  struct OpenResult {
    KOReaderProgress remoteProgress;
    bool hasRemote = false;  // true if server returned progress
    SyncStatus status = SyncStatus::SKIPPED;
  };

  // ── Skip rules ──

  /** Check all skip conditions. Returns true if sync should be skipped. */
  static bool shouldSkip(bool isOpen);

  /** Record a successful sync (resets backoff, updates timestamps). */
  static void recordSuccess(bool isOpen);

  /** Record a failed sync attempt (increments backoff). */
  static void recordFailure();

  /**
   * Record the current reading position for change detection.
   * Call after applying progress on open and after each page turn would be ideal,
   * but for simplicity call on open (after apply) and the close payload captures current.
   */
  static void recordPosition(const std::string& documentHash, int spineIndex, int page);

  /** Returns true if position has changed since last recordPosition(). */
  static bool hasPositionChanged(const std::string& documentHash, int spineIndex, int page);

  // ── Sync operations ──

  /**
   * Sync on open: fetch remote progress from KOSync server.
   * Caller must ensure WiFi is NOT needed for Epub loading (call before loadEpub).
   * Comparison and application happen in EpubReaderActivity::onEnter() after Epub loads.
   *
   * @param bookPath  Path to the epub file on SD card
   * @param wifiSsid  WiFi SSID to connect to
   * @param wifiPassword  WiFi password
   * @return OpenResult with remote progress if available
   */
  static OpenResult syncOnOpen(const std::string& bookPath, const std::string& wifiSsid,
                               const std::string& wifiPassword);

  /**
   * Sync on close: GET remote, PUT if local is ahead.
   * Caller must ensure Epub is already released (for heap headroom).
   *
   * @param payload  Precomputed from prepareClosePayload() while Epub was alive
   * @return SyncStatus for UI feedback
   */
  static SyncStatus syncOnClose(const ClosePayload& payload);

  // ── WiFi helpers ──

  /** Connect to WiFi silently. Returns true on success. */
  static bool connectWifi(const std::string& ssid, const std::string& password, uint32_t timeoutMs = 10000);

  /** Disconnect WiFi and power down radio. */
  static void disconnectWifi();

  /** Write a line to /.crosspoint/autosync.log on SD card for debugging. */
  static void logToSd(const char* message);

  // ── Session tracking ──

  /** Minimum session duration (ms) for close sync to fire. */
  static constexpr unsigned long MIN_SESSION_MS = 30000;  // 30 seconds

  /** Minimum interval between auto-syncs of the same type (ms). */
  static constexpr unsigned long MIN_SYNC_INTERVAL_MS = 120000;  // 2 minutes

  /** WiFi connect timeout for sync-on-open (fast fail when away from home). */
  static constexpr uint32_t WIFI_TIMEOUT_OPEN_MS = 5000;  // 5 seconds

  /** WiFi connect timeout for sync-on-close. */
  static constexpr uint32_t WIFI_TIMEOUT_CLOSE_MS = 5000;  // 5 seconds

  /** Maximum consecutive failures before long backoff. */
  static constexpr uint8_t MAX_FAILURES_BEFORE_LONG_BACKOFF = 3;

 private:
  // Session state (reset on every boot/wake — ESP32-C3 deep sleep is a full reboot).
  // Cooldown and backoff only apply within a single wake session.
  // Open and close have independent cooldowns so open sync doesn't block close sync.
  static unsigned long lastOpenSyncTimeMs;
  static unsigned long lastCloseSyncTimeMs;
  static uint8_t consecutiveFailures;

  // Position tracking for change detection
  static std::string lastSyncedDocHash;
  static int lastSyncedSpine;
  static int lastSyncedPage;

  /** Compute document hash based on user's preferred match method. */
  static std::string computeDocHash(const std::string& bookPath);

  /** Try NTP sync if needed (skip if time already valid). */
  static void syncNtpIfNeeded();
};
