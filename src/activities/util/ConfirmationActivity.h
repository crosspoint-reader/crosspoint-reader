#pragma once
#include <functional>
#include <string>

#include "../Activity.h"

class ConfirmationActivity : public Activity {
 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body, std::function<void(bool)> onResult);

  void onEnter() override;
  void loop() override;

 private:
  std::string heading;
  std::string body;
  std::function<void(bool)> onResult;
};