// BleSyncProtocol — Build Zero wire contract for the X4 Reading-Sync BLE service.
// Mirrors PROTOCOL.md (protocolVersion 0). UUIDs and type strings are the SINGLE
// source of truth on the firmware side; keep them identical to the iOS module.
//
// Dummy payloads only. No real book identity or reading position in Build Zero.
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace BleSyncProtocol {

constexpr int kProtocolVersion = 1;  // v1: real PROGRESS messages; v2 adds MANIFEST/WANT (same version int)

// ---- GATT UUIDs (must match PROTOCOL.md and the iOS X4SyncProtocol.swift) ----
constexpr char kServiceUuid[]        = "7ea41000-b5a3-4f21-9c7d-1a2b3c4d5e6f";
constexpr char kCharCapabilities[]   = "7ea41001-b5a3-4f21-9c7d-1a2b3c4d5e6f"; // read
constexpr char kCharX4ToPhone[]      = "7ea41002-b5a3-4f21-9c7d-1a2b3c4d5e6f"; // notify
constexpr char kCharPhoneToX4[]      = "7ea41003-b5a3-4f21-9c7d-1a2b3c4d5e6f"; // write w/ resp
constexpr char kCharSyncState[]      = "7ea41004-b5a3-4f21-9c7d-1a2b3c4d5e6f"; // read

constexpr char kAdvName[] = "X4 Sync";

// ---- Message types ----
constexpr char kTypePairHello[]     = "PAIR_HELLO";
constexpr char kTypePairAck[]       = "PAIR_ACK";
constexpr char kTypeHasUpdate[]     = "HAS_UPDATE";
constexpr char kTypeNeedsUpdate[]   = "NEEDS_UPDATE";
constexpr char kTypeAck[]           = "ACK";
constexpr char kTypeDummyPosition[] = "DUMMY_POSITION_RESPONSE";
constexpr char kTypeProgress[]      = "PROGRESS";  // v1 real reading position
constexpr char kTypeManifest[]      = "MANIFEST";  // v2 book list + per-book updatedAt
constexpr char kTypeWant[]          = "WANT";      // v2 pull: send PROGRESS for these keys

// One book row in a MANIFEST: identity + when it last changed on the sender.
// titleHash is the required match key (byte-identical both ends). updatedAt = 0
// means "have the book, no clocked progress yet" — still listed so the peer can
// seed it.
struct ManifestEntry {
  std::string titleHash;
  std::string document;    // optional docHash / epub_uid hint (never required to match)
  int64_t updatedAt = 0;   // unix seconds; 0 = unclocked / no progress
};

// A parsed inbound (phone -> X4) message. Fields absent in a given type stay empty.
struct ParsedMessage {
  bool ok = false;                 // false => JSON invalid or envelope incomplete
  int protocolVersion = -1;
  std::string messageId;
  std::string type;
  std::string source;
  std::string deviceId;
  std::string timestamp;
  // build-zero extras:
  std::string ackFor;              // ACK
  std::string dummyBookId;         // DUMMY_POSITION_RESPONSE
  float dummyPercentage = -1.0f;   // DUMMY_POSITION_RESPONSE
  std::string displayName;         // PAIR_HELLO
  // v1 PROGRESS (real reading position):
  std::string document;            // document hash
  std::string titleHash;           // v1 book_ids: MD5(norm title 0x1F author)
  std::string xpointer;            // KOReader xpointer (the "progress" field)
  float percentage = -1.0f;        // 0..1
  int64_t updatedAt = 0;           // unix seconds — conflict clock
  int64_t now = 0;                 // sender's CURRENT unix time (BLE clock sync)
  // v2 MANIFEST / WANT:
  std::vector<ManifestEntry> books;   // MANIFEST rows
  bool more = false;                  // MANIFEST paginated (more messages coming)
  std::vector<std::string> wantKeys;  // WANT: title_hashes to push
};

// Generate a v4-shaped message id from the hardware RNG.
std::string genMessageId();

// Best-effort ISO-8601 timestamp. Falls back to a boot-relative marker when the
// clock has never been NTP-synced (Build Zero does not require a real clock).
std::string nowIso();

// ---- Outbound builders (X4 -> phone), serialized JSON ----
std::string buildCapabilities();
std::string buildSyncState(bool paired, const std::string& lastPhoneId,
                           const std::string& lastEvent, const std::string& lastAck);
std::string buildHasUpdate(const std::string& deviceId, int dummyRevision);
std::string buildNeedsUpdate(const std::string& deviceId, const std::string& dummyBookId);

// v1: real reading position for the current/last book.
std::string buildProgress(const std::string& deviceId, const std::string& document,
                          const std::string& titleHash, const std::string& xpointer, float percentage,
                          int64_t updatedAt);

// v2: this device's book list (title_hash + updatedAt per book). `nowUnix` drives
// the peer's BLE clock (pass 0 to omit when our own clock is unset). `more` flags
// pagination when the manifest is split across messages.
std::string buildManifest(const std::string& deviceId, const std::vector<ManifestEntry>& books, bool more,
                          int64_t nowUnix);

// v2: ask the peer to push full PROGRESS for these title_hashes.
std::string buildWant(const std::string& deviceId, const std::vector<std::string>& keys);

// ---- Inbound parser (phone -> X4) ----
ParsedMessage parseMessage(const std::string& json);

}  // namespace BleSyncProtocol
