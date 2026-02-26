#pragma once

#include <functional>

#include "activities/ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

/**
 * Activity showing the list of configured OPDS servers.
 * Allows adding new servers and editing/deleting existing ones.
 * Used from Settings and also as a server picker from the home screen.
 */
class OpdsServerListActivity final : public ActivityWithSubactivity {
 public:
  using OnServerSelected = std::function<void(size_t serverIndex)>;

  /**
   * @param onBack Called when user presses Back
   * @param onServerSelected If set, acts as a picker: selecting a server calls this instead of opening editor.
   */
  explicit OpdsServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  const std::function<void()>& onBack, OnServerSelected onServerSelected = nullptr)
      : ActivityWithSubactivity("OpdsServerList", renderer, mappedInput),
        onBack(onBack),
        onServerSelected(std::move(onServerSelected)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  const std::function<void()> onBack;
  OnServerSelected onServerSelected;

  bool isPickerMode() const { return onServerSelected != nullptr; }
  int getItemCount() const;
  void handleSelection();
};
