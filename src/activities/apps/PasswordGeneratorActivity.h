#pragma once
#include <string>

#include "activities/Activity.h"

class PasswordGeneratorActivity final : public Activity {
 public:
  explicit PasswordGeneratorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PasswordGenerator", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { GENERATING, ENTER_SITE, ENTER_USERNAME, SAVED };

  State state = GENERATING;
  int passwordLength = 16;
  std::string generatedPassword;
  std::string site;
  std::string username;

  void generatePassword();
};
