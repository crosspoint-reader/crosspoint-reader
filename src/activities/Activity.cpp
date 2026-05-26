#include "Activity.h"

#include <numeric>

#include "ActivityManager.h"

void Activity::onEnter() {
  entryPressedMask = 0;
  static constexpr MappedInputManager::Button kGuarded[] = {
      MappedInputManager::Button::Back,  MappedInputManager::Button::Confirm, MappedInputManager::Button::Left,
      MappedInputManager::Button::Right, MappedInputManager::Button::Up,      MappedInputManager::Button::Down,
  };
  entryPressedMask = std::accumulate(
      std::begin(kGuarded), std::end(kGuarded), uint8_t{0}, [&](uint8_t mask, MappedInputManager::Button b) -> uint8_t {
        return mappedInput.isPressed(b) ? mask | static_cast<uint8_t>(1u << static_cast<uint8_t>(b)) : mask;
      });
  LOG_DBG("ACT", "Entering activity: %s", name.c_str());
}

bool Activity::wasReleasedGuarded(MappedInputManager::Button button) const {
  const uint8_t mask = static_cast<uint8_t>(1u << static_cast<uint8_t>(button));
  if (entryPressedMask & mask) {
    if (!mappedInput.isPressed(button)) {
      entryPressedMask &= ~mask;
    }
    return false;
  }
  return mappedInput.wasReleased(button);
}

void Activity::onExit() { LOG_DBG("ACT", "Exiting activity: %s", name.c_str()); }

void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

void Activity::requestUpdateAndWait() { activityManager.requestUpdateAndWait(); }

void Activity::onGoHome(HomeMenuItem item) { activityManager.goHome(item); }

void Activity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }
