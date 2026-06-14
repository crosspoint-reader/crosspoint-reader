#pragma once

#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Memory.h>

#include <memory>
#include <string>
#include <vector>

#include "../Activity.h"
#include "WordRef.h"
#include "util/ButtonNavigator.h"

/// Bitmask flags controlling how a word is rendered (cursor or selection).
/// Flags are combinable: e.g. FILL | UNDERLINE draws a gray background with an underline.
/// When both FILL and INVERT are set, INVERT takes priority (solid black over dither).
struct ClipWordStyle {
  enum Flags : uint8_t {
    NONE = 0,
    FILL = 1 << 0,       ///< fillRectDither background using fillColor
    INVERT = 1 << 1,     ///< solid black background with white text
    UNDERLINE = 1 << 2,  ///< horizontal line at font baseline
    BORDER = 1 << 3,     ///< drawRect outline around the word
  };
  uint8_t flags = FILL;
  Color fillColor = Color::LightGray;  ///< used only when FILL flag is set
};

/// Visual configuration for the selection UI.
struct ClipRenderConfig {
  ClipWordStyle cursor;     ///< style of the word under the cursor
  ClipWordStyle selection;  ///< style of words in the selected range
  bool showButtonHints = true;
  HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH;
};

struct ClipSelectionConfig {
  enum class Mode {
    /// Two-step confirm: first sets start mark, second confirms range.
    /// Returns ClippingResult with full anchor metadata.
    CLIPPING,

    /// Two-step confirm: first sets start mark, second confirms range.
    /// Returns WordSelectResult with text only — no anchor building.
    /// Single-word select: confirm twice on the same word.
    WORD_SELECT,
  };

  Mode mode = Mode::CLIPPING;
  ClipRenderConfig render;
};

class ClipSelectionActivity final : public Activity {
 public:
  using WordStyle = ClipWordStyle;
  using RenderConfig = ClipRenderConfig;
  using Config = ClipSelectionConfig;

  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::vector<WordRef> words,
                        std::string bookTitle, std::string author, std::string chapterTitle, int pageNumber, int fontId,
                        Section& section, int startPageInSection, int marginTop, int marginLeft,
                        Config config = Config{});

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }

 private:
  std::vector<WordRef> words;
  std::string bookTitle;
  std::string author;
  std::string chapterTitle;
  int pageNumber;
  int fontId;
  Config config;

  Section& section;
  int startPageInSection;
  int marginTop;
  int marginLeft;

  std::unique_ptr<uint8_t[]> savedBuffer;
  size_t savedBufferSize = 0;
  int currentDisplayPage = 0;
  int savedSectionPage = 0;

  int cursorIdx = 0;
  int startMarkIdx = -1;
  bool needsPageSwitch = false;

  ButtonNavigator buttonNavigator;

  void switchToPage(int pageIdx);
  void drawHighlights();
  void applyWordStyle(const WordRef& word, const WordStyle& style) const;
  int lineEndForward(int idx) const;
  int lineEndBackward(int idx) const;
};
