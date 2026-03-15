#pragma once
#include <I18n.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"
#include "util/Dictionary.h"

class DictionarySelectActivity final : public Activity {
 public:
  explicit DictionarySelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DictionarySelect", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Discovered dictionary folder names (not including "None" sentinel).
  std::vector<std::string> dictFolders;

  // Index into the full list including "None" at position 0.
  int selectedIndex = 0;
  int totalItems = 0;

  // Whether we are showing .ifo info for the currently highlighted dictionary.
  bool showingInfo = false;
  DictInfo currentInfo;

  // Suppresses the Confirm release that bleeds through from the parent activity launch.
  bool ignoreNextConfirmRelease = false;

  ButtonNavigator buttonNavigator;

  // Scans /dictionary/ on the SD card and populates dictFolders.
  void scanDictionaries();

  // Returns the folder path for a given list index (0 = None → empty string).
  std::string folderForIndex(int index) const;

  // Returns the display name for a given list index.
  const char* nameForIndex(int index) const;

  // Applies the currently highlighted selection to settings and Dictionary.
  void applySelection();
};
