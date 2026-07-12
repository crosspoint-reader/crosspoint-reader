#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class KeyCopierActivity final : public Activity {
 public:
  explicit KeyCopierActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("KeyCopier", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { EDIT, TYPE_SELECT, SAVED };
  State state = EDIT;

  struct KeyType {
    const char* name;
    int numCuts;
    int minDepth;
    int maxDepth;
  };

  static constexpr KeyType KEY_TYPES[] = {
      {"Kwikset KW1",  5, 1, 7},
      {"Schlage SC1",  5, 0, 9},
      {"Yale Y1",      5, 1, 9},
      {"Custom 6-pin", 6, 0, 9},
      {"Custom 7-pin", 7, 0, 9},
  };
  static constexpr int KEY_TYPE_COUNT = 5;

  int typeIndex = 0;    // selected key type
  int cutIndex = 0;     // selected cut position (0-based)
  int cuts[8] = {};     // cut depths, one per position

  // type select list cursor
  int typeSelectIndex = 0;

  const KeyType& kt() const { return KEY_TYPES[typeIndex]; }

  void drawKey() const;
  void drawCutSelector() const;
  void drawTypeMenu() const;

  void saveKey() const;
  void loadKey();
};
