#pragma once
#include <cstdint>
#include <vector>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EventLoggerActivity final : public Activity {
 public:
  explicit EventLoggerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("EventLogger", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { LOG_LIST, VIEW_ENTRY, COMPOSE };
  State state = LOG_LIST;

  struct LogEntry {
    uint64_t timestampMs;
    char text[128];
  };

  static constexpr int MAX_ENTRIES = 50;
  static constexpr const char* LOG_PATH = "/biscuit/logs/eventlog.csv";

  std::vector<LogEntry> entries;
  int selectedIndex = 0;

  void loadEntries();
  void saveEntry(const char* text);
  void renderList() const;
  void renderView() const;
  static void formatTimestamp(uint64_t ms, char* buf, int len);
};
