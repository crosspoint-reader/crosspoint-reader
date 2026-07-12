#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class PasswordManagerActivity final : public Activity {
 public:
  explicit PasswordManagerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PasswordManager", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;

  int getItemCount() const;
};
