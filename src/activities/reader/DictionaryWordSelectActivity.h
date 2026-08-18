#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/Dictionary.h"

// Word selection over the current reader page: Left/Right step through words
// in reading order, Up/Down jump rows, Confirm looks the word up and opens
// DictionaryDefinitionActivity, Back returns to the reader. On touch devices a
// touch-down moves the highlight and a tap on a word looks it up directly.
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Redraws the reader's page (word boxes over it), so it follows the reading
  // surface's night-mode polarity; a normal-polarity flash mid-lookup jars.
  bool appliesNightMode() const override { return true; }

 private:
  // Screen box of one selectable word. `text` points into the owned Page's
  // TextBlock arena (NUL-terminated), valid for this activity's lifetime.
  struct WordBox {
    int16_t x;
    int16_t y;
    int16_t width;
    uint16_t row;
    const char* text;
    EpdFontFamily::Style style;
  };

  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  // Hold threshold below which a two-button release counts as a word-step tap
  // rather than the start of row-jump repeat.
  static constexpr uint16_t WORD_STEP_MAX_HOLD_MS = 500;

  void extractWords();
  int closestInRow(uint16_t row, int centerX) const;
  int wordAt(int x, int y) const;
  void moveVertical(int direction);
  // Steps the selection by `direction` words in reading order (bounds-checked),
  // shared by four-button Left/Right and two-button navigation.
  void moveWord(int direction);
  // Reads SETTINGS.getDictWordNavIntervalMs() at activity-construction time, so
  // wordNavigator's const repeat interval reflects the current speed setting
  // without pulling CrossPointSettings.h into this header.
  static uint16_t dictWordNavIntervalMs();
  void performLookup();
  bool drawHighlightWithSnapshot();
  void drawHints() const;

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  int fontId = 0;
  int lineHeight = 0;

  std::vector<WordBox> words;
  int selected = 0;
  uint16_t rowCount = 0;

  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  bool dictNeedsIndex = false;

  // Two-button navigation (Up+Left = previous, Down+Right = next): a tap steps
  // one word, holding repeats as row jumps. Only its continuous-repeat half is
  // used here (see loop()); the row-jump step reuses the same interval-timing
  // mechanism DictionaryDefinitionActivity uses for page turns. Unused in
  // four-button mode.
  ButtonNavigator wordNavigator{dictWordNavIntervalMs(), WORD_STEP_MAX_HOLD_MS};
  // Button groups for wordNavigator.onContinuous(), held as members rather
  // than passed as brace-init lists at each call site: onContinuous() takes
  // its list by const std::vector&, so a literal there would allocate a
  // temporary heap buffer every tick. These allocate once, at construction.
  // (ButtonNavigator::Buttons itself is a private alias, so this spells out
  // the underlying std::vector<Button> it names.)
  const std::vector<MappedInputManager::Button> prevRowJumpButtons{MappedInputManager::Button::ScreenUp,
                                                                   MappedInputManager::Button::ScreenLeft};
  const std::vector<MappedInputManager::Button> nextRowJumpButtons{MappedInputManager::Button::ScreenDown,
                                                                   MappedInputManager::Button::ScreenRight};

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;

  // Differential highlight repaint: the pixels under the current highlight
  // box, so a cursor move restores them and repaints only the two affected
  // boxes instead of re-running the full two-pass page render (which also
  // reloads every SD-font glyph on the page). snapshotIdx is the word whose
  // under-pixels are saved; -1 means the framebuffer no longer holds a clean
  // page (popup drawn, sub-activity shown) and the next render must be full.
  static constexpr size_t SNAPSHOT_CAPACITY = 4096;
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotIdx = -1;
};
