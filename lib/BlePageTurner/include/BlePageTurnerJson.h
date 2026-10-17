#pragma once

#include <ArduinoJson.h>

#include <cstring>

#include "BlePageTurner.h"

// The page turner's settings in the host's settings.json, under the keys blePageTurnerEnabled,
// blePick, blePeerAddr, blePeerName, blePrevKeyUsage, bleNextKeyUsage and bleRemotes (an array of at
// most 4 {addr, binds[<= 8]}). Files written before these keys load unchanged. Inline: the
// host's own settings code is the one caller, and compiled there the JSON helpers inline as
// they did before this moved into the module.
namespace bleturner {
namespace detail {
inline void copyField(char* dest, const char* src, const size_t maxLen) {
  if (dest != src) strncpy(dest, src, maxLen - 1);
  dest[maxLen - 1] = '\0';
}
}  // namespace detail

inline void writeJson(const Config& config, JsonDocument& doc) {
  doc["blePageTurnerEnabled"] = config.enabled;
  doc["blePick"] = config.pick == 0 ? 0 : 1;
  doc["blePeerAddr"] = config.peerAddr;
  doc["blePeerName"] = config.peerName;
  doc["blePrevKeyUsage"] = config.prevKeyUsage;
  doc["bleNextKeyUsage"] = config.nextKeyUsage;
  // One entry per remote: its address and its 32-bit slots. An empty table is written too:
  // the user cleared it, and the built-in default must not come back.
  if (config.remoteCount > 0) {
    JsonArray remotes = doc["bleRemotes"].to<JsonArray>();
    for (uint8_t i = 0; i < config.remoteCount && i < kMaxRemotes; i++) {
      JsonObject r = remotes.add<JsonObject>();
      r["addr"] = config.remotes[i].addr;
      JsonArray binds = r["binds"].to<JsonArray>();
      for (uint8_t j = 0; j < config.remotes[i].count && j < kMaxBindings; j++)
        binds.add(config.remotes[i].bindings[j]);
    }
  }
}

// Returns false when the file has no blePageTurnerEnabled key (written before the page
// turner existed): the host saves once so the keys are there from then on.
inline bool readJson(Config& config, JsonVariantConst doc) {
  // A file without the keys means nobody used the page turner: the defaults stay (off, no
  // remote, nothing learned).
  config.enabled = (doc["blePageTurnerEnabled"] | uint8_t{0}) ? 1 : 0;
  config.pick = doc["blePick"].is<uint8_t>() && doc["blePick"].as<uint8_t>() == 0 ? 0 : 1;
  detail::copyField(config.peerAddr, doc["blePeerAddr"] | "", sizeof(config.peerAddr));
  detail::copyField(config.peerName, doc["blePeerName"] | "", sizeof(config.peerName));
  config.prevKeyUsage = doc["blePrevKeyUsage"] | uint8_t{0};
  config.nextKeyUsage = doc["bleNextKeyUsage"] | uint8_t{0};
  config.remoteCount = 0;
  for (const JsonObjectConst r : doc["bleRemotes"].as<JsonArrayConst>()) {
    if (config.remoteCount >= kMaxRemotes) break;
    const char* addr = r["addr"] | "";
    if (addr[0] == '\0') continue;
    // Build the entry aside first: the document's string may point into this very slot.
    RemoteTable t{};
    detail::copyField(t.addr, addr, sizeof(t.addr));
    for (const JsonVariantConst b : r["binds"].as<JsonArrayConst>()) {
      if (t.count >= kMaxBindings) break;
      // A slot is kept only when every bit means something (BleKeyBinding.h valid()).
      if (b.is<uint32_t>() && valid(b.as<uint32_t>())) t.bindings[t.count++] = b.as<uint32_t>();
    }
    config.remotes[config.remoteCount++] = t;
  }
  return !doc["blePageTurnerEnabled"].isNull();
}

}  // namespace bleturner
