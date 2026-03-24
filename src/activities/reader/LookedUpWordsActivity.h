#pragma once
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/DictionaryLookupController.h"
#include "util/LookupHistory.h"

class LookedUpWordsActivity final : public Activity {
 public:
  explicit LookedUpWordsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string cachePath,
                                 int readerFontId)
      : Activity("LookedUpWords", renderer, mappedInput),
        cachePath(std::move(cachePath)),
        readerFontId(readerFontId),
        controller(renderer, mappedInput, *this) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string cachePath;
  int readerFontId;
  std::vector<LookupHistory::Entry> entries;
  int selectedIndex = 0;
  bool deleteConfirmMode = false;

  DictionaryLookupController controller;

  static constexpr unsigned long LONG_PRESS_MS = 600;

  bool skipLoopDelay() override { return controller.skipLoopDelay(); }

  // Convert UI index (0 = most recent) to 0-based file index (0 = oldest).
  int fileIndexOf(int uiIndex) const { return static_cast<int>(entries.size()) - 1 - uiIndex; }

  static const char* glyphFor(LookupHistory::Status s);
};
