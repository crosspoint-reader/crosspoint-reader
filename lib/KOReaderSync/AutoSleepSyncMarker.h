#pragma once
#include <string>

#include "AutoSleepSyncPolicy.h"

/**
 * SD persistence for the per-book last-synced-position marker.
 *
 * The marker lives in the book's existing cache directory and is written only
 * after a successful KOReader sync (upload, already-synced, or remote-applied).
 * An eligible sleep whose reader position still matches the marker skips the
 * whole network preflight and commits sleep immediately.
 *
 * One small write per successful sync (value-change guarded); a missing or
 * unreadable marker simply means the sync runs, so failures degrade safely.
 */
class AutoSleepSyncMarker {
 public:
  // Book cache directory for an EPUB path. Must stay in lockstep with the
  // derivation in Epub's constructor (lib/Epub/Epub.h); a divergence only
  // causes the skip to never trigger, never a wrong skip.
  static std::string bookCachePathFor(const std::string& epubPath);

  // Hash of server URL + username + match method; a change invalidates markers.
  static uint32_t serverFingerprint();

  static bool load(const std::string& bookCachePath, AutoSleepSyncMarkerData& out);
  static void save(const std::string& bookCachePath, const AutoSleepSyncMarkerData& data);

 private:
  static constexpr uint8_t FILE_VERSION = 1;
  // version byte + fingerprint + spine + page + totalPages
  static constexpr size_t FILE_SIZE = 1 + 4 + 2 + 2 + 2;
};
