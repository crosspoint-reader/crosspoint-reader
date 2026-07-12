#include "HabitTrackerActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <cstring>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void HabitTrackerActivity::load() {
  memset(&data, 0, sizeof(data));
  auto file = Storage.open(SAVE_PATH);
  if (file) {
    file.read(reinterpret_cast<uint8_t*>(&data), sizeof(data));
    file.close();
  }
}

void HabitTrackerActivity::save() {
  Storage.mkdir("/biscuit");
  auto file = Storage.open(SAVE_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (file) {
    file.write(reinterpret_cast<const uint8_t*>(&data), sizeof(data));
    file.close();
  }
}

void HabitTrackerActivity::onEnter() {
  Activity::onEnter();
  load();

  // New session: advance sessionId, reset completedToday for habits that missed last session
  data.sessionId++;

  for (int i = 0; i < data.habitCount; i++) {
    Habit& h = data.habits[i];
    // If habit was not completed in the last session, reset streak
    if (h.lastSessionId < data.sessionId - 1 && h.lastSessionId != 0) {
      h.streak = 0;
    }
    h.completedToday = false;
  }

  save();
  state = TODAY_VIEW;
  selectedIndex = 0;
  requestUpdate();
}

void HabitTrackerActivity::onExit() { Activity::onExit(); }

void HabitTrackerActivity::loop() {
  if (state == TODAY_VIEW) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { finish(); return; }

    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, data.habitCount);
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, data.habitCount);
      requestUpdate();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && data.habitCount > 0) {
      Habit& h = data.habits[selectedIndex];
      h.completedToday = !h.completedToday;
      if (h.completedToday) {
        h.streak++;
        if (h.streak > h.bestStreak) h.bestStreak = h.streak;
        h.lastSessionId = data.sessionId;
      } else {
        if (h.streak > 0) h.streak--;
      }
      save();
      requestUpdate();
    }

    // Long-press Right → edit habits
    if (mappedInput.wasReleased(MappedInputManager::Button::Right) &&
        mappedInput.getHeldTime() >= 500) {
      state = EDIT_HABITS;
      selectedIndex = 0;
      requestUpdate();
    }
    return;
  }

  if (state == EDIT_HABITS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = TODAY_VIEW; selectedIndex = 0; requestUpdate(); return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      int total = data.habitCount + 1;
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, total);
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      int total = data.habitCount + 1;
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, total);
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (selectedIndex == data.habitCount) {
        // Add habit
        if (data.habitCount >= MAX_HABITS) return;
        startActivityForResult(
            std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Habit Name", "", 31),
            [this](const ActivityResult& r) {
              if (!r.isCancelled && data.habitCount < MAX_HABITS) {
                const auto& text = std::get<KeyboardResult>(r.data).text;
                Habit& h = data.habits[data.habitCount];
                memset(&h, 0, sizeof(h));
                strncpy(h.name, text.c_str(), sizeof(h.name) - 1);
                data.habitCount++;
                save();
              }
            });
      } else {
        // Delete selected habit
        for (int i = selectedIndex; i < data.habitCount - 1; i++) {
          data.habits[i] = data.habits[i + 1];
        }
        data.habitCount--;
        if (selectedIndex >= data.habitCount && selectedIndex > 0) selectedIndex--;
        save();
        requestUpdate();
      }
    }
    return;
  }
}

void HabitTrackerActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Habit Tracker");

  if (state == TODAY_VIEW) renderToday();
  else renderEditList();

  renderer.displayBuffer();
}

void HabitTrackerActivity::renderToday() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listH = pageHeight - listTop - metrics.buttonHintsHeight;

  if (data.habitCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, listTop + listH / 2, "Hold Right to add habits");
  } else {
    GUI.drawList(renderer, Rect{0, listTop, pageWidth, listH}, data.habitCount, selectedIndex,
      [this](int i) -> std::string {
        return std::string(data.habits[i].completedToday ? "[x] " : "[ ] ") + data.habits[i].name;
      },
      [this](int i) -> std::string {
        char buf[32];
        snprintf(buf, sizeof(buf), "Streak: %d  Best: %d", data.habits[i].streak, data.habits[i].bestStreak);
        return buf;
      });
  }

  const auto labels = mappedInput.mapLabels("Back", "Toggle", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void HabitTrackerActivity::renderEditList() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listH = pageHeight - listTop - metrics.buttonHintsHeight;
  const int total = data.habitCount + 1;

  GUI.drawList(renderer, Rect{0, listTop, pageWidth, listH}, total, selectedIndex,
    [this](int i) -> std::string {
      if (i < data.habitCount) return std::string("[-] ") + data.habits[i].name;
      return "+ Add Habit";
    });

  const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
