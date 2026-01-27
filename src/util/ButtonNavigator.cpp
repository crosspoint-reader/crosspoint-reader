#include "ButtonNavigator.h"

const MappedInputManager* ButtonNavigator::mappedInput = nullptr;

void ButtonNavigator::onNext(const Callback& callback) {
  onNextPress(callback);
  onNextContinuous(callback);
}
void ButtonNavigator::onPrevious(const Callback& callback) {
  onPreviousPress(callback);
  onPreviousContinuous(callback);
}

void ButtonNavigator::onNextPress(const Callback& callback) { onPress(getNextButtons(), callback); }

void ButtonNavigator::onPreviousPress(const Callback& callback) { onPress(getPreviousButtons(), callback); }

void ButtonNavigator::onNextContinuous(const Callback& callback) { onContinuous(getNextButtons(), callback); }

void ButtonNavigator::onPreviousContinuous(const Callback& callback) { onContinuous(getPreviousButtons(), callback); }

void ButtonNavigator::onPress(const Buttons& buttons, const Callback& callback) {
  if (!mappedInput) return;

  bool buttonPressed = false;
  for (const MappedInputManager::Button button : buttons) {
    if (mappedInput->wasPressed(button)) {
      buttonPressed = true;
      break;
    }
  }

  if (buttonPressed && !recentlyNavigatedContinuously()) {
    callback();
  }
}

void ButtonNavigator::onContinuous(const Buttons& buttons, const Callback& callback) {
  if (!mappedInput) return;

  bool buttonPressed = false;
  for (const MappedInputManager::Button button : buttons) {
    if (mappedInput->isPressed(button)) {
      buttonPressed = true;
      break;
    }
  }

  if (buttonPressed && shouldNavigateContinuously()) {
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

bool ButtonNavigator::recentlyNavigatedContinuously() const {
  const int elapsedTime = millis() - lastContinuousNavTime;
  if (elapsedTime < 50) {
    return true;
  }

  return false;
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