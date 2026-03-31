#pragma once

#include <GfxRenderer.h>

#include "../Activity.h"
#include "DictTypes.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;

class DictionaryDefinitionActivity final : public Activity {
 public:
  // Takes ownership of heap-allocated results array (freed in destructor).
  DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* word,
                               DictResult* results, int resultCount);
  ~DictionaryDefinitionActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  char searchedWord[64];
  DictResult* results = nullptr;  // Heap-allocated, owned
  int resultCount = 0;
  int currentResult = 0;
  int scrollOffset = 0;     // Line offset for paginated scrolling
  int totalLines = 0;       // Computed during render for scroll clamping
  int maxVisibleLines = 0;  // Computed during render for scroll clamping
  ButtonNavigator buttonNavigator;

  void drawDefinition(int contentTop, int contentLeft, int contentWidth, int contentHeight);
};
