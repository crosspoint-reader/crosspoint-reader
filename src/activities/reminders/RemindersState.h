#pragma once
#include <cstdint>
#include <ctime>

// Data model for the Reminders (Taskpoint) feature.
//
// A single fixed-size array of CalItem holds the merged Google Calendar events
// and incomplete Google Tasks. The array is statically sized (no heap growth) to
// keep DRAM pressure predictable on the ESP32-C3 (see CLAUDE.md "Resource
// Protocol"): the whole struct lives as a single global, gRemindersData.
static constexpr uint8_t REMINDERS_MAX_ITEMS = 16;
static constexpr uint8_t REMINDERS_MAX_NOTES = 3;

struct CalItem {
  char title[80];
  char location[64];                    // first comma-separated portion only
  char notes[REMINDERS_MAX_NOTES][48];  // task notes / sub-items
  uint8_t note_count;
  time_t start_epoch;   // UTC seconds; 0 = no specific time (all-day / undated)
  time_t end_epoch;     // UTC seconds; 0 = unknown
  int32_t travel_secs;  // 0 = unknown (reserved for future Maps integration)
  bool is_calendar;     // true = Calendar event, false = Google Task
  bool completed;
  bool all_day;  // true = date-only (all-day event or dated task); start_epoch is that day's UTC midnight
};

struct RemindersData {
  CalItem items[REMINDERS_MAX_ITEMS];
  uint8_t count;
  time_t synced_epoch;  // when the last successful sync completed (UTC)
  bool is_stale;        // set once the live 5-minute window has elapsed

  // Reset to an empty, never-synced state.
  void clear();

  // Append one item; returns false if the array is already full.
  bool addItem(const CalItem& item);

  // Sort items in place by start_epoch ascending (undated items last).
  void sortByStart();

  // Persist to / restore from /.crosspoint/reminders_cache.json so the sleep
  // screen and a cold boot can render the last-known list without a network sync.
  bool saveToFile() const;
  bool loadFromFile();

  static constexpr char CACHE_PATH[] = "/.crosspoint/reminders_cache.json";
};

extern RemindersData gRemindersData;
