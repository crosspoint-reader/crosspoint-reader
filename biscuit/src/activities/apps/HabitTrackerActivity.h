#pragma once
#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class HabitTrackerActivity final : public Activity {
 public:
  explicit HabitTrackerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("HabitTracker", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { TODAY_VIEW, EDIT_HABITS };
  State state = TODAY_VIEW;

  static constexpr int MAX_HABITS = 10;
  static constexpr const char* SAVE_PATH = "/biscuit/habits.dat";

  struct Habit {
    char name[32];
    bool completedToday;
    int streak;
    int bestStreak;
    uint32_t lastSessionId;
  };

  struct SaveData {
    uint32_t sessionId;
    int habitCount;
    Habit habits[MAX_HABITS];
  };

  SaveData data = {};
  int selectedIndex = 0;

  void load();
  void save();
  void renderToday() const;
  void renderEditList() const;
};
