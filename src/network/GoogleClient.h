#pragma once

#include "activities/reminders/RemindersState.h"

/**
 * GoogleClient — minimal Google Calendar + Tasks reader for the Taskpoint
 * Reminders feature.
 *
 * Uses the OAuth2 refresh-token flow (Device Authorization Flow set up once on a
 * PC; see firmware/FIRMWARE_SETUP.md). Credentials live on the SD card at
 * /.crosspoint/google_creds.json and are loaded on every sync.
 *
 * syncAll() performs the full sequence (WiFi connect -> NTP -> token refresh ->
 * Calendar GET -> Tasks GET -> WiFi teardown) and fills `out`. HTTPS is verified
 * against the firmware's bundled CA roots via esp_crt_bundle_attach (the same
 * transport HttpDownloader uses), so no per-cert maintenance is required.
 */
class GoogleClient {
 public:
  enum class Result {
    OK,
    NoCredentials,  // google_creds.json missing or malformed
    WifiFailed,     // could not join the last-known network
    ClockUnset,     // NTP failed and the system clock is implausible (countdowns would be garbage)
    AuthFailed,     // token endpoint rejected the refresh token
    FetchFailed,    // both Calendar and Tasks calls failed
    Cancelled,      // caller requested abort via the cancel flag
  };

  // Connects WiFi, syncs the clock, fetches Calendar + Tasks, and tears WiFi
  // back down. On OK, `out` holds the merged, start-sorted item list and a fresh
  // synced_epoch. On failure `out` is left untouched so a cached list survives.
  //
  // `cancel`, when non-null, is polled at phase boundaries and inside the WiFi
  // wait and HTTP read loops; setting it true aborts the sync promptly with
  // Result::Cancelled.
  static Result syncAll(RemindersData& out, const volatile bool* cancel = nullptr);

  // Human-readable label for logging / UI.
  static const char* resultName(Result r);
};
