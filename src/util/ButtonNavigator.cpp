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
  const bool wasReleased = std::any_of(buttons.begin(), buttons.end(), [](const MappedInputManager::Button button) {
    return mappedInput != nullptr && mappedInput->wasReleased(button);
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
