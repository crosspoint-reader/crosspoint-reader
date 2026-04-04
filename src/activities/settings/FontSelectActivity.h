#pragma once

#include <functional>

#include "activities/ActivityWithSubactivity.h"

/**
 * Font selection page
 * Shows available external fonts and allows user to select
 * Uses synchronous rendering (no background task) to avoid FreeRTOS conflicts
 */
class FontSelectActivity final : public ActivityWithSubactivity {
 public:
  enum class SelectMode { Reader, UI };
  explicit FontSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, SelectMode mode,
                              const std::function<void()>& onBack)
      : ActivityWithSubactivity("FontSelect", renderer, mappedInput), mode(mode), onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  SelectMode mode;
  int selectedIndex = 0;  // Index in the current list
  int totalItems = 1;     // At least one built-in option
  const std::function<void()> onBack;

  void render();
  void handleSelection();
};
