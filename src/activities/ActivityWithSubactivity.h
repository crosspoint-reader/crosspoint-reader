#pragma once
#include <memory>

#include "Activity.h"

class ActivityWithSubactivity : public Activity {
 protected:
  std::unique_ptr<Activity> subActivity = nullptr;
  void exitActivity();
  void enterNewActivity(Activity* activity);

 public:
  explicit ActivityWithSubactivity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity(std::move(name), renderer, mappedInput) {}
  void loop() override;
  // Note: when a subactivity is active, any calls to requestUpdate() will be forwarded to the subactivity.
  // This will effectively pause the parent activity's rendering until the subactivity is exited.
  void requestUpdate() override;
  void onExit() override;
};
