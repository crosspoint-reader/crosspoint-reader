#pragma once
#include <memory>

#include "Activity.h"

class ActivityWithSubactivity : public Activity {
 protected:
  std::unique_ptr<Activity> subActivity = nullptr;
  void exitActivity();
  void enterNewActivity(Activity* activity);
  [[noreturn]] void renderTaskLoop() override;

 public:
  explicit ActivityWithSubactivity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity(std::move(name), renderer, mappedInput) {}
  void loop() override;
  // Note: when a subactivity is active, parent requestUpdate() calls are ignored;
  // the subactivity should request its own renders. This pauses parent rendering until exit.
  void requestUpdate() override;
  void onExit() override;
};
