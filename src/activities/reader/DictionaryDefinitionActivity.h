#pragma once
#include <EpdFontFamily.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/DictionaryLookupController.h"
#include "util/IpaUtils.h"
#include "util/LookupHistory.h"
#include "util/WordSelectNavigator.h"

class DictionaryDefinitionActivity final : public Activity {
 public:
  // showLookupButton=true:
  //   Confirm = enter word-select mode on the definition text (Look Up Word).
  //   Back (short press) = return to caller (isCancelled=true).
  //   Back (long press, >= LONG_PRESS_MS) = Done — exit to reader (isCancelled=false).
  // showLookupButton=false:
  //   Back/Confirm both return to caller (isCancelled=true). Unchanged from old behaviour.
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const std::string& headword, const DictLocation& location,
                                        bool showLookupButton = false, std::string bookCachePath = "",
                                        bool recordHistory = false, std::string historyWord = "",
                                        LookupHistory::Status historyStatus = LookupHistory::Status::NotFound)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(headword),
        foundLocation(location),
        showLookupButton(showLookupButton),
        cachePath(std::move(bookCachePath)),
        recordHistory(recordHistory),
        historyWord(std::move(historyWord)),
        historyStatus(historyStatus),
        controller(renderer, mappedInput, *this, cachePath) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string headword;
  DictLocation foundLocation;
  bool showLookupButton;
  std::string cachePath;
  bool recordHistory;
  std::string historyWord;
  LookupHistory::Status historyStatus;
  std::vector<std::string> chainWords;  // previous headwords for back-nav
  bool chainBackNavInProgress = false;

  // A single styled run within a display line.
  struct LayoutSegment {
    std::string text;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    bool isIpa = false;  // true → render with IPA_FONT_ID
  };

  // One wrapped display line, containing one or more styled segments.
  struct LayoutLine {
    std::vector<LayoutSegment> segments;
    uint8_t indentLevel = 0;
    bool isListItem = false;
  };

  std::vector<LayoutLine> layoutLines;
  int currentPage = 0;
  int linesPerPage = 0;
  int totalPages = 0;

  // Orientation-aware layout gutters (computed in wrapText, used in render and extractWordsFromLayout)
  int leftPadding = 20;
  int rightPadding = 20;
  int hintGutterHeight = 0;
  int contentX = 0;
  int hintGutterWidth = 0;
  int bodyStartY = 0;  // top of the text body (set in wrapText)

  // Word-select mode (activated by pressing Look Up Word in view mode)
  bool isWordSelectMode = false;
  WordSelectNavigator navigator;
  DictionaryLookupController controller;

  // Differential repaint state for in-definition word-select mode. Only consulted
  // when isWordSelectMode is true; reset on every view-mode render.
  enum class RenderMode { FullPage, Differential };
  RenderMode nextRenderMode_ = RenderMode::FullPage;
  int prevHighlightIdx_ = -1;

  bool skipLoopDelay() override { return controller.skipLoopDelay(); }

  void wrapText();
  void wrapHtml();
  void wrapPlain();
  void extractWordsFromLayout();
  int getMixedWidth(std::vector<IpaTextSpan>& ipaRuns, const char* text, EpdFontFamily::Style style);
  bool handleLongPressExitAll(bool enabled);
  int getLineHeight() const;
};
