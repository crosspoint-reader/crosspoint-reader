#pragma once
#include "activities/Activity.h"

class PasswordDetailActivity final : public Activity {
 public:
  explicit PasswordDetailActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int entryIndex)
      : Activity("PasswordDetail", renderer, mappedInput), entryIndex(entryIndex) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { VIEWING, CONFIRM_DELETE, DELETED };

  int entryIndex;
  State state = VIEWING;
  bool showPassword = false;
};
