#pragma once
#include <I18n.h>

#include <string>

#include "activities/Activity.h"

class QrDisplayActivity final : public Activity {
 public:
  explicit QrDisplayActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& textPayload,
                             const std::function<void()>& onGoBack)
      : Activity("QrDisplay", renderer, mappedInput), textPayload(textPayload), onGoBack(onGoBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  std::string textPayload;
  const std::function<void()> onGoBack;
};
