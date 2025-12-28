#pragma once
#include <memory>

#include "Activity.h"

class ActivityWithSubactivity : public Activity {
 protected:
  std::unique_ptr<Activity> subActivity = nullptr;
  void exitActivity();
  void enterNewActivity(Activity* activity);

 public:
  explicit ActivityWithSubactivity(std::string name, GfxRenderer& renderer, InputManager& inputManager)
      : Activity(std::move(name), renderer, inputManager) {}
  void loop() override;
  void onExit() override;
};
