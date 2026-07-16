// BleProgressBridge — real reading-position read/apply for BLE sync.
//
// Pure reuse of CrossPoint's existing KOReader-sync machinery: the same document
// hash (KOReaderDocumentId), the same position mapping (ProgressMapper), the same
// persistence (EpubReaderUtils::saveProgress / progress.bin). BLE just carries the
// existing KOReaderProgress payload instead of HTTP. No new sync logic.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "BleSyncProtocol.h"     // ManifestEntry
#include "KOReaderSyncClient.h"  // KOReaderProgress payload

class GfxRenderer;

namespace BleProgress {

// Document hash of the current/last-read book (path-only; empty if none).
std::string currentDocumentHash();

// Fill `out` with the current/last-read book's saved position as a
// KOReaderProgress, plus `titleHash` (MD5 of normalized title+author — the
// optimization-proof match key). Returns false if there is no last book, it
// can't be loaded, or it has no saved progress yet.
bool getLocal(KOReaderProgress& out, std::string& titleHash, const std::string& deviceId);

// Like getLocal but for an arbitrary book path (v2 multi-book). False if the
// book can't be loaded or has no saved progress yet.
bool getForPath(const std::string& path, KOReaderProgress& out, std::string& titleHash, const std::string& deviceId);

// v2: this device's manifest — most-recent-first, capped at maxN. Built from
// RECENT_BOOKS (title_hash from cached title/author, no epub load) + each book's
// progress-time.bin. updatedAt = 0 for books with no clocked progress yet.
std::vector<BleSyncProtocol::ManifestEntry> buildLocalManifest(size_t maxN);

// (path, titleHash) for every known book with a title — cheap, cached metadata
// only (no epub load). Lets a WiFi client match its own library against the X4
// by the optimization-proof title hash even when filenames differ.
std::vector<std::pair<std::string, std::string>> pathTitleHashes(size_t maxN);

// v2: resolve an incoming book (document hash and/or title_hash) to a local book
// path via RECENT_BOOKS. "" when no recent book matches.
std::string pathForHash(const std::string& document, const std::string& titleHash);

// v2: last-save unix time for the recent book matching `titleHash` (0 if unknown).
int64_t localTimeForHash(const std::string& titleHash);

// Positions saved while the clock was unset carry an explicit 0 stamp in
// progress-time.bin (EpubReaderUtils::saveProgress). Once BLE NTP sets the
// clock, this rewrites those 0 markers to `now` so positions the user just
// read stop losing newest-wins. Books with NO stamp file are untouched
// (never read with sync on — must not claim freshness). Returns the number
// of books backfilled.
size_t backfillUnclockedTimestamps(int64_t now);

// Apply a received KOReaderProgress to the matching local book. Resolves the
// target by document hash OR `remoteTitleHash` across RECENT_BOOKS (v2: not just
// the open book). Newest-wins. Writes progress.bin (apply-on-next-open); never
// mutates a book being rendered. Returns false on mismatch/older/failure.
bool applyRemote(const KOReaderProgress& in, const std::string& remoteTitleHash, GfxRenderer& renderer);

// MD5 of normalized (title 0x1F author) for the current/last book, or "".
std::string currentTitleHash();

}  // namespace BleProgress
