#include "BleSyncProtocol.h"

#include <ArduinoJson.h>  // v7 (same as firmware's KOReaderSyncClient)
#include <esp_random.h>

#include <cmath>
#include <cstdio>
#include <ctime>

#include "BleClock.h"

namespace BleSyncProtocol {

std::string genMessageId() {
  uint8_t b[16];
  for (int i = 0; i < 16; i++) b[i] = static_cast<uint8_t>(esp_random() & 0xFF);
  b[6] = (b[6] & 0x0F) | 0x40;  // version 4
  b[8] = (b[8] & 0x3F) | 0x80;  // variant 10xx
  char out[37];
  std::snprintf(out, sizeof(out), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return std::string(out);
}

std::string nowIso() {
  const time_t t = time(nullptr);
  // 2001-09-09 = 1000000000; treat anything earlier as "clock never set".
  if (t < 1000000000) return "1970-01-01T00:00:00Z";
  struct tm tm_utc;
  gmtime_r(&t, &tm_utc);
  char out[25];
  std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  return std::string(out);
}

// Fill the shared envelope on an outbound document.
static void envelope(JsonDocument& doc, const char* type, const std::string& deviceId) {
  doc["protocolVersion"] = kProtocolVersion;
  doc["messageId"] = genMessageId();
  doc["type"] = type;
  doc["source"] = "x4";
  doc["deviceId"] = deviceId;
  doc["timestamp"] = nowIso();
}

std::string buildCapabilities() {
  JsonDocument doc;
  doc["protocol"] = kProtocolVersion;
  doc["device"] = "xteink-x4";
  doc["firmware"] = "crosspoint-ble-v1";
  JsonArray supports = doc["supports"].to<JsonArray>();
  supports.add("progress");
  supports.add("manifest");
  supports.add("want");
  supports.add("peer_clock");
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::string buildSyncState(bool paired, const std::string& lastPhoneId, const std::string& lastEvent,
                           const std::string& lastAck) {
  JsonDocument doc;
  doc["paired"] = paired;
  doc["lastPeerId"] = lastPhoneId;
  doc["lastPhoneId"] = lastPhoneId;
  doc["lastEvent"] = lastEvent;
  doc["lastAck"] = lastAck;
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::string buildHasUpdate(const std::string& deviceId, int dummyRevision) {
  JsonDocument doc;
  envelope(doc, kTypeHasUpdate, deviceId);
  doc["dummyRevision"] = dummyRevision;
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::string buildNeedsUpdate(const std::string& deviceId, const std::string& dummyBookId) {
  JsonDocument doc;
  envelope(doc, kTypeNeedsUpdate, deviceId);
  doc["dummyBookId"] = dummyBookId;
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::string buildProgress(const std::string& deviceId, const std::string& document, const std::string& titleHash,
                          const std::string& xpointer, float percentage, int64_t updatedAt) {
  JsonDocument doc;
  envelope(doc, kTypeProgress, deviceId);
  doc["document"] = document;
  doc["titleHash"] = titleHash;
  doc["progress"] = xpointer;
  doc["percentage"] = percentage;
  doc["updatedAt"] = updatedAt;
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::string buildManifest(const std::string& deviceId, const std::vector<ManifestEntry>& books, bool more,
                          int64_t nowUnix) {
  JsonDocument doc;
  envelope(doc, kTypeManifest, deviceId);
  if (nowUnix > 1000000000) doc["now"] = nowUnix;  // omit when our clock is unset
  doc["more"] = more;
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& b : books) {
    JsonObject o = arr.add<JsonObject>();
    o["titleHash"] = b.titleHash;
    if (!b.document.empty()) o["document"] = b.document;
    o["updatedAt"] = b.updatedAt;
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::string buildWant(const std::string& deviceId, const std::vector<std::string>& keys) {
  JsonDocument doc;
  envelope(doc, kTypeWant, deviceId);
  JsonArray arr = doc["keys"].to<JsonArray>();
  for (const auto& k : keys) arr.add(k);
  std::string out;
  serializeJson(doc, out);
  return out;
}

ParsedMessage parseMessage(const std::string& json) {
  ParsedMessage m;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json);
  if (err) return m;  // m.ok stays false

  m.protocolVersion = doc["protocolVersion"] | -1;
  m.messageId = doc["messageId"] | "";
  m.type = doc["type"] | "";
  m.source = doc["source"] | "";
  m.deviceId = doc["deviceId"] | "";
  m.timestamp = doc["timestamp"] | "";
  m.ackFor = doc["ackFor"] | "";
  m.dummyBookId = doc["dummyBookId"] | "";
  m.displayName = doc["displayName"] | "";
  // v1 PROGRESS fields:
  m.document = doc["document"] | "";
  m.titleHash = doc["titleHash"] | "";
  m.xpointer = doc["progress"] | "";
  m.percentage = doc["percentage"] | -1.0f;
  m.updatedAt = doc["updatedAt"] | (int64_t)0;
  m.now = doc["now"] | (int64_t)0;
  if (doc["dummyPosition"].is<JsonObject>()) {
    m.dummyPercentage = doc["dummyPosition"]["percentage"] | -1.0f;
  }
  const JsonArray manifestRows = doc["books"].as<JsonArray>();
  bool manifestRowsValid = !manifestRows.isNull() && manifestRows.size() <= 20;
  // v2 MANIFEST rows.
  m.more = doc["more"] | false;
  if (manifestRowsValid) {
    m.books.reserve(manifestRows.size());
    for (JsonObject o : manifestRows) {
      ManifestEntry e;
      e.titleHash = o["titleHash"] | "";
      e.document = o["document"] | "";
      e.updatedAt = o["updatedAt"] | (int64_t)0;
      if ((e.titleHash.empty() && e.document.empty()) || e.updatedAt < 0) {
        manifestRowsValid = false;
      } else {
        m.books.push_back(std::move(e));
      }
    }
  }
  // v2 WANT keys.
  const JsonArray wantKeys = doc["keys"].as<JsonArray>();
  bool wantKeysValid = !wantKeys.isNull() && wantKeys.size() <= 20;
  if (wantKeysValid) {
    m.wantKeys.reserve(wantKeys.size());
    for (JsonVariant k : wantKeys) {
      const std::string s = k | "";
      if (s.empty()) {
        wantKeysValid = false;
      } else {
        m.wantKeys.push_back(s);
      }
    }
  }

  // BLE writes are an external input boundary. Require the shared envelope and
  // the fields that make each handled message safe to dispatch.
  const bool envelopeValid = m.protocolVersion == kProtocolVersion && !m.messageId.empty() && !m.type.empty() &&
                             !m.source.empty() && !m.deviceId.empty();
  bool payloadValid = false;
  if (m.type == kTypePairHello || m.type == kTypePairAck) {
    payloadValid = true;
  } else if (m.type == kTypeAck) {
    payloadValid = !m.ackFor.empty();
  } else if (m.type == kTypeDummyPosition) {
    payloadValid = !m.dummyBookId.empty() && std::isfinite(m.dummyPercentage) && m.dummyPercentage >= 0.0f &&
                   m.dummyPercentage <= 1.0f;
  } else if (m.type == kTypeProgress) {
    const bool hasIdentity = !m.document.empty() || !m.titleHash.empty();
    payloadValid =
        hasIdentity && std::isfinite(m.percentage) && m.percentage >= 0.0f && m.percentage <= 1.0f && m.updatedAt >= 0;
  } else if (m.type == kTypeManifest) {
    // Peer time is optional for interoperability, but when supplied it must be
    // an integer in the same supported range used by the clock consumer.
    const JsonVariantConst peerNow = doc["now"];
    const bool peerNowValid = peerNow.isNull() || (peerNow.is<int64_t>() && BleClock::isValidPeerTime(m.now));
    payloadValid = manifestRowsValid && doc["more"].is<bool>() && peerNowValid;
  } else if (m.type == kTypeWant) {
    payloadValid = wantKeysValid && !m.wantKeys.empty();
  }
  m.ok = envelopeValid && payloadValid;
  return m;
}

}  // namespace BleSyncProtocol
