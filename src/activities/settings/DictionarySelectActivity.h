#pragma once
#include <I18n.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"
#include "util/Dictionary.h"

class DictionarySelectActivity final : public Activity {
 public:
  explicit DictionarySelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                    std::string bookCachePath = "")
      : Activity("DictionarySelect", renderer, mappedInput), bookCachePath(std::move(bookCachePath)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Active SD card root directory for dictionaries (resolved in scanDictionaries()).
  std::string dictRoot;

  // Discovered dictionary folder names and file stems (parallel vectors, excluding "None").
  // e.g. dictFolders[i] = "dict-en-en", dictStems[i] = "dict-data"
  // folderForIndex() combines them with dictRoot into the full base path used for file access.
  std::vector<std::string> dictFolders;
  std::vector<std::string> dictStems;

  // Index into the full list including "None" at position 0.
  int selectedIndex = 0;
  int totalItems = 0;

  // Whether we are showing the metadata info screen for the highlighted dictionary.
  bool showingInfo = false;
  // When showingInfo is true: false = parsed field view, true = raw .ifo text view.
  bool showingRaw = false;
  // Parsed metadata from the .ifo file (populated on long-press).
  DictInfo currentInfo;
  // Suppresses the Confirm release that bleeds through from the parent activity launch.
  bool ignoreNextConfirmRelease = false;

  // Non-empty when launched from reader menu (per-book override mode).
  std::string bookCachePath;
  // Active per-book dictionary path loaded on enter; empty = "Use Global".
  std::string currentBookDictPath;
  // Augmented "Use Global" label showing the global dict name, e.g. "Use Global (dict-en-en)".
  // Only populated in per-book mode.
  std::string useGlobalLabel;

  ButtonNavigator buttonNavigator;

  // Scans the first available dictionary root directory on the SD card and populates dictFolders.
  void scanDictionaries();

  // Returns the folder path for a given list index (0 = None → empty string).
  std::string folderForIndex(int index) const;

  // Returns the display name for a given list index.
  const char* nameForIndex(int index) const;

  // Applies the currently highlighted selection to settings and Dictionary.
  void applySelection();
};
