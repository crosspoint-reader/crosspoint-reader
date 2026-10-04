#pragma once
#include <I18n.h>

#include <functional>
#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

class ConfirmationActivity : public Activity {
 private:
  // Input data
  std::string heading;
  std::string body;
  StrId cancelLabel;
  StrId confirmLabel;

  OptionPopup confirmPopup;

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body, StrId cancelLabel = StrId::STR_CANCEL,
                       StrId confirmLabel = StrId::STR_CONFIRM);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};
