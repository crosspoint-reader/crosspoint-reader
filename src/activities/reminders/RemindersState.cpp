#include "RemindersState.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

constexpr char RemindersData::CACHE_PATH[];

// Single global instance (see RemindersState.h rationale).
RemindersData gRemindersData = {};

void RemindersData::clear() {
  count = 0;
  synced_epoch = 0;
  is_stale = false;
  memset(items, 0, sizeof(items));
}

bool RemindersData::addItem(const CalItem& item) {
  if (count >= REMINDERS_MAX_ITEMS) return false;
  items[count++] = item;
  return true;
}

void RemindersData::sortByStart() {
  // Stable sort: timed items first (ascending), undated (start_epoch == 0) last.
  std::stable_sort(items, items + count, [](const CalItem& a, const CalItem& b) {
    const bool aUndated = a.start_epoch == 0;
    const bool bUndated = b.start_epoch == 0;
    if (aUndated != bUndated) return !aUndated;  // dated before undated
    return a.start_epoch < b.start_epoch;
  });
}

bool RemindersData::saveToFile() const {
  // Build the JSON in RAM, then write once. The document is small (<=16 items)
  // and short-lived, so it is freed as soon as this function returns.
  JsonDocument doc;
  doc["synced_epoch"] = static_cast<int64_t>(synced_epoch);
  doc["is_stale"] = is_stale;
  JsonArray arr = doc["items"].to<JsonArray>();
  for (uint8_t i = 0; i < count; i++) {
    const CalItem& it = items[i];
    JsonObject o = arr.add<JsonObject>();
    o["title"] = it.title;
    o["location"] = it.location;
    o["task_id"] = it.task_id;
    o["start"] = static_cast<int64_t>(it.start_epoch);
    o["end"] = static_cast<int64_t>(it.end_epoch);
    o["travel"] = it.travel_secs;
    o["cal"] = it.is_calendar;
    o["done"] = it.completed;
    o["allday"] = it.all_day;
    if (it.note_count > 0) {
      JsonArray notes = o["notes"].to<JsonArray>();
      for (uint8_t n = 0; n < it.note_count && n < REMINDERS_MAX_NOTES; n++) notes.add(it.notes[n]);
    }
  }

  HalFile file;
  if (!Storage.openFileForWrite("RMND", CACHE_PATH, file)) {
    LOG_ERR("RMND", "Failed to open cache for write: %s", CACHE_PATH);
    return false;
  }
  // HalFile is a Print, so ArduinoJson can serialize straight into it.
  if (serializeJson(doc, file) == 0) {
    LOG_ERR("RMND", "Failed to serialize reminders cache");
    return false;
  }
  LOG_DBG("RMND", "Saved %u items to cache", count);
  return true;
}

bool RemindersData::loadFromFile() {
  if (!Storage.exists(CACHE_PATH)) {
    LOG_DBG("RMND", "No reminders cache present");
    return false;
  }
  // The cache is small; read it whole, then parse. readFile returns an empty
  // String on failure, which deserializeJson rejects below.
  const String content = Storage.readFile(CACHE_PATH);
  if (content.isEmpty()) {
    LOG_ERR("RMND", "Reminders cache empty/unreadable");
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, content.c_str());
  if (err) {
    LOG_ERR("RMND", "Cache parse error: %s", err.c_str());
    return false;
  }

  clear();
  synced_epoch = static_cast<time_t>(doc["synced_epoch"] | 0LL);  // cppcheck-suppress badBitmaskCheck
  is_stale = doc["is_stale"] | false;                             // cppcheck-suppress badBitmaskCheck

  for (JsonObject o : doc["items"].as<JsonArray>()) {
    if (count >= REMINDERS_MAX_ITEMS) break;
    CalItem it = {};
    snprintf(it.title, sizeof(it.title), "%s", o["title"] | "");
    snprintf(it.location, sizeof(it.location), "%s", o["location"] | "");
    snprintf(it.task_id, sizeof(it.task_id), "%s", o["task_id"] | "");
    it.start_epoch = static_cast<time_t>(o["start"] | 0LL);  // cppcheck-suppress badBitmaskCheck
    it.end_epoch = static_cast<time_t>(o["end"] | 0LL);      // cppcheck-suppress badBitmaskCheck
    it.travel_secs = o["travel"] | 0;                        // cppcheck-suppress badBitmaskCheck
    it.is_calendar = o["cal"] | false;                       // cppcheck-suppress badBitmaskCheck
    it.completed = o["done"] | false;                        // cppcheck-suppress badBitmaskCheck
    it.all_day = o["allday"] | false;                        // cppcheck-suppress badBitmaskCheck
    it.note_count = 0;
    for (JsonVariant n : o["notes"].as<JsonArray>()) {
      if (it.note_count >= REMINDERS_MAX_NOTES) break;
      const char* ns = n.as<const char*>();
      snprintf(it.notes[it.note_count], sizeof(it.notes[0]), "%s", ns ? ns : "");
      it.note_count++;
    }
    items[count++] = it;
  }
  LOG_DBG("RMND", "Loaded %u items from cache", count);
  return true;
}
