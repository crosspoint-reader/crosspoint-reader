#pragma once

#include <DictionaryManager.h>
#include <GfxRenderer.h>
#include <LookupHistory.h>

#include "../Activity.h"
#include "DictTypes.h"

// Lightweight word position info for definition text selection mode.
// Intentionally separate from WordInfo (EPUB page layout) — simpler struct, different context.
struct DefWordInfo {
  int16_t x, y;        // Position on screen
  int16_t width;       // Pixel width of the word
  int16_t textOffset;  // Byte offset into definition text
  int16_t textLen;     // Byte length of the word
};

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

  // --- Chained lookup word stack ---
  static constexpr int MAX_CHAIN_DEPTH = 25;
  char wordStack[MAX_CHAIN_DEPTH][DICT_WORD_MAX];  // ~800 bytes
  int stackDepth = 0;

  // --- Word selection mode ---
  static constexpr int MAX_DEF_WORDS = 200;
  DefWordInfo defWords[MAX_DEF_WORDS];
  int defWordCount = 0;
  bool selectionMode = false;
  int selectedWordIndex = 0;
  bool confirmLongPressConsumed = false;

  // --- Long-press Back ---
  static constexpr unsigned long LONG_PRESS_MS = 1000;
  bool backLongPressConsumed = false;

  // --- Chained lookup support ---
  DictionaryManager dictManager;
  LookupHistory lookupHistory;

  // --- Stale-release absorption ---
  bool ignoreNextBackRelease = false;
  bool ignoreNextConfirmRelease = false;

  void performChainedLookup(const char* word);
  void popStack();
  void renderSelectionHighlight();
  int findWordOnAdjacentLine(int currentIdx, int targetY) const;

  void drawDefinition(int contentTop, int contentLeft, int contentWidth, int contentHeight, int rightGutter);
};
