#pragma once

#include <functional>
#include <string>

#include "../Activity.h"

class PromptActivity final : public Activity {
 public:
  PromptActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string message,
                 std::function<void()> onConfirm, std::function<void()> onCancel);

  void onEnter() override;
  void loop() override;
  void render(Activity::RenderLock&& lock) override;

 private:
  std::string message;
  std::function<void()> onConfirm;
  std::function<void()> onCancel;
};