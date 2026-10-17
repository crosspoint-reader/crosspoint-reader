#pragma once

#include <ArduinoJson.h>
#include <Logging.h>

#include <string>

#include "KOReaderSyncClient.h"

// What a 2xx reply to GET /syncs/progress/<document> says, given a body that is not empty: OK with
// outProgress filled in when it holds a record, NOT_FOUND when it is one of the shapes a server
// answers "nothing stored" with, JSON_ERROR when it does not parse. Kept apart from the transport
// so the host tests can feed it server replies. Inline, and included only by KOReaderSyncClient.cpp:
// in a translation unit of its own it cost 1.7 KB of flash, ArduinoJson's serializer and
// deserializer being compiled a second time.
inline KOReaderSyncClient::Error parseProgressReply(const std::string& body, const std::string& documentHash,
                                                    KOReaderProgress& outProgress) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, body);

  if (error) {
    LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
    return KOReaderSyncClient::JSON_ERROR;
  }

  // kosync convention: no stored progress for the document is signalled by a 2xx
  // carrying an empty payload, not by 404. What "empty" serializes to varies by
  // implementation — "{}", "[]", "\"\"", or a bare null — so anything that is not an
  // object carrying a position means no record. Detect that so the caller doesn't
  // apply a zeroed-out position as if it were real progress.
  //
  // A position is an XPath in "progress", or a percentage on its own. Calibre-Web NextGen
  // serves what its web reader and a Kobo report as "progress": null plus a percentage (the
  // GET asks for those, see getProgress()), and ProgressMapper places a record without an
  // XPath by its percentage. Requiring "progress" read every such record as "nothing stored":
  // a pull found nothing, and smart sync uploaded instead of applying it. Its reply naming a
  // position it withholds, {"position_kinds_available":[...]}, holds neither. Strings are
  // read through `| ""`: as<std::string>() turns a missing field into "null".
  const char* const progress = doc["progress"] | "";
  const bool hasPercentage = doc["percentage"].is<float>();
  if (!doc.is<JsonObject>() || (progress[0] == '\0' && !hasPercentage)) {
    std::string jsonDump;
    serializeJson(doc, jsonDump);
    LOG_INF("KOSync", "Empty progress payload — treating as not found | payload=%s", jsonDump.c_str());
    return KOReaderSyncClient::NOT_FOUND;
  }

  outProgress.document = documentHash;
  outProgress.progress = progress;
  outProgress.percentage = doc["percentage"].as<float>();
  outProgress.device = doc["device"] | "";
  outProgress.deviceId = doc["device_id"] | "";
  outProgress.timestamp = doc["timestamp"].as<int64_t>();

  LOG_INF("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
  return KOReaderSyncClient::OK;
}
