#pragma once

#include <functional>
#include <vector>

#include "MappedInputManager.h"

class ButtonNavigator final {
  using Callback = std::function<void()>;
  using Buttons = std::vector<MappedInputManager::Button>;

  const uint16_t continuousStartMs;
  const uint16_t continuousIntervalMs;
  uint32_t lastContinuousNavTime = 0;
  static const MappedInputManager* mappedInput;
  std::function<bool(int)> selectablePredicate;
  int selectableTotalItems = 0;

  uint32_t lastNextPressMs = 0;
  uint32_t lastPreviousPressMs = 0;
  bool longPressNextFired = false;
  bool longPressPreviousFired = false;
  int indexBeforePress = 0;

  static constexpr uint16_t listDoubleClickMs = 350;
  static constexpr uint32_t listLongPressMs = 1500;
  static constexpr int listJumpCount = 10;

  [[nodiscard]] bool shouldNavigateContinuously() const;
  void onListNav(const Buttons& buttons, bool forward, int& selectedIndex, int totalItems, uint32_t& lastPressMs,
                 bool& longPressFired, const Callback& onChange);

 public:
  explicit ButtonNavigator(const uint16_t continuousIntervalMs = 500, const uint16_t continuousStartMs = 500)
      : continuousStartMs(continuousStartMs), continuousIntervalMs(continuousIntervalMs) {}

  static void setMappedInputManager(const MappedInputManager& mappedInputManager) { mappedInput = &mappedInputManager; }

  void onNext(const Callback& callback);
  void onPrevious(const Callback& callback);
  void onPressAndContinuous(const Buttons& buttons, const Callback& callback);

  void onNextPress(const Callback& callback);
  void onPreviousPress(const Callback& callback);
  void onPress(const Buttons& buttons, const Callback& callback);

  void onNextRelease(const Callback& callback);
  void onPreviousRelease(const Callback& callback);
  void onRelease(const Buttons& buttons, const Callback& callback);

  void onNextContinuous(const Callback& callback);
  void onPreviousContinuous(const Callback& callback);
  void onContinuous(const Buttons& buttons, const Callback& callback);

  [[nodiscard]] static int nextIndex(int currentIndex, int totalItems);
  [[nodiscard]] static int previousIndex(int currentIndex, int totalItems);
  [[nodiscard]] static int nextIndex(int currentIndex, const std::vector<bool>& selectable);
  [[nodiscard]] static int previousIndex(int currentIndex, const std::vector<bool>& selectable);
  [[nodiscard]] static int nextIndex(int currentIndex, int totalItems,
                                     const std::function<bool(int index)>& isSelectable);
  [[nodiscard]] static int previousIndex(int currentIndex, int totalItems,
                                         const std::function<bool(int index)>& isSelectable);

  [[nodiscard]] int nextIndex(int currentIndex) const;
  [[nodiscard]] int previousIndex(int currentIndex) const;
  void setSelectablePredicate(std::function<bool(int)> selectablePredicate, int totalItems);
  void clearSelectablePredicate();

  [[nodiscard]] static int nextPageIndex(int currentIndex, int totalItems, int itemsPerPage);
  [[nodiscard]] static int previousPageIndex(int currentIndex, int totalItems, int itemsPerPage);

  // List navigation with double-click (skip 10) and long-press (jump to edge).
  // Replaces the typical onNext/onPrevious + onContinuous pattern for drawList consumers.
  void onNextList(int& selectedIndex, int totalItems, const Callback& onChange);
  void onNextList(const Buttons& buttons, int& selectedIndex, int totalItems, const Callback& onChange);
  void onPreviousList(int& selectedIndex, int totalItems, const Callback& onChange);
  void onPreviousList(const Buttons& buttons, int& selectedIndex, int totalItems, const Callback& onChange);

  [[nodiscard]] static Buttons getNextButtons() {
    return {MappedInputManager::Button::Down, MappedInputManager::Button::Right};
  }
  [[nodiscard]] static Buttons getPreviousButtons() {
    return {MappedInputManager::Button::Up, MappedInputManager::Button::Left};
  }
};