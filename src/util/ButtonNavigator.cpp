#include "ButtonNavigator.h"

#include "ButtonEventManager.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"

const MappedInputManager* ButtonNavigator::mappedInput = nullptr;

void ButtonNavigator::onNext(const Callback& callback) {
  onNextPress(callback);
  onNextContinuous(callback);
}

void ButtonNavigator::onPrevious(const Callback& callback) {
  onPreviousPress(callback);
  onPreviousContinuous(callback);
}

void ButtonNavigator::onPressAndContinuous(const Buttons& buttons, const Callback& callback) {
  onPress(buttons, callback);
  onContinuous(buttons, callback);
}

void ButtonNavigator::onNextPress(const Callback& callback) { onPress(getNextButtons(), callback); }

void ButtonNavigator::onPreviousPress(const Callback& callback) { onPress(getPreviousButtons(), callback); }

void ButtonNavigator::onNextRelease(const Callback& callback) { onRelease(getNextButtons(), callback); }

void ButtonNavigator::onPreviousRelease(const Callback& callback) { onRelease(getPreviousButtons(), callback); }

void ButtonNavigator::onNextContinuous(const Callback& callback) { onContinuous(getNextButtons(), callback); }

void ButtonNavigator::onPreviousContinuous(const Callback& callback) { onContinuous(getPreviousButtons(), callback); }

void ButtonNavigator::onPress(const Buttons& buttons, const Callback& callback) {
  const bool wasPressed = std::any_of(buttons.begin(), buttons.end(), [](const MappedInputManager::Button button) {
    return mappedInput != nullptr && mappedInput->wasPressed(button);
  });

  if (wasPressed) {
    callback();
  }
}

void ButtonNavigator::onRelease(const Buttons& buttons, const Callback& callback) {
  // The double-click FSM in ButtonEventManager delays Short events by DOUBLE_WINDOW_MS
  // (300ms) when a double-press action is configured for that button, so the configured
  // Short and Double actions can be disambiguated. In a reader activity that gating must
  // suppress release-based navigation during the wait, otherwise Left/Right would both
  // turn the page AND fire the configured short action.
  //
  // In non-reader UIs (settings, file browser, etc.), only navigation reacts to release —
  // the configured per-button actions are not dispatched there (see
  // ActivityManager::dispatchButtonAction, which is reader-only). Gating on isShortPending
  // there just makes Left/Right navigation feel sluggish (300ms lag) compared to Up/Down
  // (which have no FSM at all). So skip the gate outside reader activities.
  const bool inReader = activityManager.isReaderActivity();
  const bool wasReleased =
      std::any_of(buttons.begin(), buttons.end(), [inReader](const MappedInputManager::Button button) {
        if (mappedInput == nullptr || !mappedInput->wasReleased(button)) {
          return false;
        }
        return !(inReader && globalButtonEvents().isShortPending(button));
      });

  if (wasReleased) {
    if (lastContinuousNavTime == 0) {
      callback();
    }

    lastContinuousNavTime = 0;
  }
}

void ButtonNavigator::onContinuous(const Buttons& buttons, const Callback& callback) {
  const bool isPressed = std::any_of(buttons.begin(), buttons.end(), [this](const MappedInputManager::Button button) {
    return mappedInput != nullptr && mappedInput->isPressed(button) && shouldNavigateContinuously();
  });

  if (isPressed) {
    callback();
    lastContinuousNavTime = millis();
  }
}

bool ButtonNavigator::shouldNavigateContinuously() const {
  if (!mappedInput) return false;

  const bool buttonHeldLongEnough = mappedInput->getHeldTime() > continuousStartMs;
  const bool navigationIntervalElapsed = (millis() - lastContinuousNavTime) > continuousIntervalMs;

  return buttonHeldLongEnough && navigationIntervalElapsed;
}

void ButtonNavigator::setSelectablePredicate(std::function<bool(int)> selectablePredicate, int totalItems) {
  this->selectablePredicate = std::move(selectablePredicate);
  this->selectableTotalItems = totalItems;
}

void ButtonNavigator::clearSelectablePredicate() {
  selectablePredicate = nullptr;
  selectableTotalItems = 0;
}

int ButtonNavigator::nextIndex(int currentIndex) const {
  if (!selectablePredicate || selectableTotalItems <= 0) return currentIndex;
  return nextIndex(currentIndex, selectableTotalItems, selectablePredicate);
}

int ButtonNavigator::previousIndex(int currentIndex) const {
  if (!selectablePredicate || selectableTotalItems <= 0) return currentIndex;
  return previousIndex(currentIndex, selectableTotalItems, selectablePredicate);
}

int ButtonNavigator::nextIndex(const int currentIndex, const int totalItems) {
  if (totalItems <= 0) return 0;

  // Calculate the next index with wrap-around
  return (currentIndex + 1) % totalItems;
}

int ButtonNavigator::previousIndex(const int currentIndex, const int totalItems) {
  if (totalItems <= 0) return 0;

  // Calculate the previous index with wrap-around
  return (currentIndex + totalItems - 1) % totalItems;
}

int ButtonNavigator::nextIndex(const int currentIndex, const std::vector<bool>& selectable) {
  const int totalItems = static_cast<int>(selectable.size());
  if (totalItems <= 0) return 0;

  int index = nextIndex(currentIndex, totalItems);
  for (int i = 0; i < totalItems; ++i) {
    if (selectable[index]) {
      return index;
    }
    index = nextIndex(index, totalItems);
  }

  return currentIndex;
}

int ButtonNavigator::previousIndex(const int currentIndex, const std::vector<bool>& selectable) {
  const int totalItems = static_cast<int>(selectable.size());
  if (totalItems <= 0) return 0;

  int index = previousIndex(currentIndex, totalItems);
  for (int i = 0; i < totalItems; ++i) {
    if (selectable[index]) {
      return index;
    }
    index = previousIndex(index, totalItems);
  }

  return currentIndex;
}

int ButtonNavigator::nextIndex(const int currentIndex, const int totalItems,
                               const std::function<bool(int index)>& isSelectable) {
  if (totalItems <= 0) return 0;
  if (!isSelectable) return nextIndex(currentIndex, totalItems);

  int index = nextIndex(currentIndex, totalItems);
  for (int i = 0; i < totalItems; ++i) {
    if (isSelectable(index)) {
      return index;
    }
    index = nextIndex(index, totalItems);
  }

  return currentIndex;
}

int ButtonNavigator::previousIndex(const int currentIndex, const int totalItems,
                                   const std::function<bool(int index)>& isSelectable) {
  if (totalItems <= 0) return 0;
  if (!isSelectable) return previousIndex(currentIndex, totalItems);

  int index = previousIndex(currentIndex, totalItems);
  for (int i = 0; i < totalItems; ++i) {
    if (isSelectable(index)) {
      return index;
    }
    index = previousIndex(index, totalItems);
  }

  return currentIndex;
}

int ButtonNavigator::nextPageIndex(const int currentIndex, const int totalItems, const int itemsPerPage) {
  if (totalItems <= 0 || itemsPerPage <= 0) return 0;

  // When items fit on one page, use index navigation instead
  if (totalItems <= itemsPerPage) {
    return nextIndex(currentIndex, totalItems);
  }

  const int lastPageIndex = (totalItems - 1) / itemsPerPage;
  const int currentPageIndex = currentIndex / itemsPerPage;

  if (currentPageIndex < lastPageIndex) {
    return (currentPageIndex + 1) * itemsPerPage;
  }

  return 0;
}

int ButtonNavigator::previousPageIndex(const int currentIndex, const int totalItems, const int itemsPerPage) {
  if (totalItems <= 0 || itemsPerPage <= 0) return 0;

  // When items fit on one page, use index navigation instead
  if (totalItems <= itemsPerPage) {
    return previousIndex(currentIndex, totalItems);
  }

  const int lastPageIndex = (totalItems - 1) / itemsPerPage;
  const int currentPageIndex = currentIndex / itemsPerPage;

  if (currentPageIndex > 0) {
    return (currentPageIndex - 1) * itemsPerPage;
  }

  return lastPageIndex * itemsPerPage;
}

void ButtonNavigator::onNextList(int& selectedIndex, const int totalItems, const Callback& onChange) {
  onListNav(getNextButtons(), true, selectedIndex, totalItems, lastNextPressMs, longPressNextFired, onChange);
}

void ButtonNavigator::onNextList(const Buttons& buttons, int& selectedIndex, const int totalItems,
                                 const Callback& onChange) {
  onListNav(buttons, true, selectedIndex, totalItems, lastNextPressMs, longPressNextFired, onChange);
}

void ButtonNavigator::onPreviousList(int& selectedIndex, const int totalItems, const Callback& onChange) {
  onListNav(getPreviousButtons(), false, selectedIndex, totalItems, lastPreviousPressMs, longPressPreviousFired,
            onChange);
}

void ButtonNavigator::onPreviousList(const Buttons& buttons, int& selectedIndex, const int totalItems,
                                     const Callback& onChange) {
  onListNav(buttons, false, selectedIndex, totalItems, lastPreviousPressMs, longPressPreviousFired, onChange);
}

void ButtonNavigator::onListNav(const Buttons& buttons, const bool forward, int& selectedIndex, const int totalItems,
                                uint32_t& lastPressMs, bool& longPressFired, const Callback& onChange) {
  if (!mappedInput || totalItems <= 0) return;

  const bool anyHeld = std::any_of(buttons.begin(), buttons.end(),
                                   [](const MappedInputManager::Button b) { return mappedInput->isPressed(b); });

  if (anyHeld && mappedInput->getHeldTime() > listLongPressMs) {
    if (!longPressFired) {
      longPressFired = true;
      if (forward) {
        selectedIndex = totalItems - 1;
        if (selectablePredicate) {
          while (selectedIndex > 0 && !selectablePredicate(selectedIndex)) --selectedIndex;
        }
      } else {
        selectedIndex = 0;
        if (selectablePredicate) {
          while (selectedIndex < totalItems - 1 && !selectablePredicate(selectedIndex)) ++selectedIndex;
        }
      }
      onChange();
    }
    return;
  }

  const bool wasReleased = std::any_of(buttons.begin(), buttons.end(),
                                       [](const MappedInputManager::Button b) { return mappedInput->wasReleased(b); });
  if (wasReleased) {
    longPressFired = false;
    return;
  }

  const bool wasPressed = std::any_of(buttons.begin(), buttons.end(),
                                      [](const MappedInputManager::Button b) { return mappedInput->wasPressed(b); });
  if (!wasPressed) return;

  const uint32_t now = millis();
  const bool isDouble = (now - lastPressMs) < listDoubleClickMs;
  lastPressMs = now;

  if (isDouble) {
    // Restore to position before the first press so the total movement is exactly listJumpCount.
    // Guard against a stale indexBeforePress if totalItems shrank since the single press stored it.
    selectedIndex = (indexBeforePress >= 0 && indexBeforePress < totalItems) ? indexBeforePress : selectedIndex;
    for (int i = 0; i < listJumpCount; ++i) {
      const int next =
          forward ? (selectablePredicate ? nextIndex(selectedIndex) : nextIndex(selectedIndex, totalItems))
                  : (selectablePredicate ? previousIndex(selectedIndex) : previousIndex(selectedIndex, totalItems));
      // Stop before wrapping: forward movement decreases index only on wrap; backward vice versa.
      if (forward && next <= selectedIndex) break;
      if (!forward && next >= selectedIndex) break;
      selectedIndex = next;
    }
  } else {
    indexBeforePress = selectedIndex;
    selectedIndex =
        forward ? (selectablePredicate ? nextIndex(selectedIndex) : nextIndex(selectedIndex, totalItems))
                : (selectablePredicate ? previousIndex(selectedIndex) : previousIndex(selectedIndex, totalItems));
  }
  onChange();
}
