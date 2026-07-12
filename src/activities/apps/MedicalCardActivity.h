#pragma once
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class MedicalCardActivity final : public Activity {
 public:
  explicit MedicalCardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("MedicalCard", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { CARD_DISPLAY, EDIT_SELECT, QR_VIEW };

  State state = CARD_DISPLAY;
  ButtonNavigator buttonNavigator;
  int fieldIndex = 0;

  static constexpr int FIELD_COUNT = 7;
  struct MedicalInfo {
    char name[32] = "";
    char bloodType[8] = "";
    char allergies[128] = "";
    char medications[128] = "";
    char conditions[128] = "";
    char emergencyContact[64] = "";
    char emergencyPhone[20] = "";
  };

  MedicalInfo info;
  static constexpr const char* SAVE_PATH = "/biscuit/medical.dat";

  void loadFromSd();
  void saveToSd();
  void renderDisplay() const;
  void renderEditSelect() const;
  void renderQrView() const;

  static const char* fieldLabel(int index);
  char* fieldPtr(int index);
  size_t fieldMaxLen(int index);
};
