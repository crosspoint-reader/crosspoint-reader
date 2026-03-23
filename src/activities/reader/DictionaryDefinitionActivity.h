#pragma once
#include <EpdFontFamily.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/DictionaryLookupController.h"
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
                                        const std::string& headword, const std::string& definition, int readerFontId,
                                        bool showLookupButton = false)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(headword),
        definition(definition),
        readerFontId(readerFontId),
        showLookupButton(showLookupButton),
        controller(renderer, mappedInput, *this) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string headword;
  std::string definition;
  int readerFontId;
  bool showLookupButton;

  // A single styled run within a display line.
  struct LayoutSegment {
    std::string text;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
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
  bool inMultiSelectMode = false;
  int anchorFlatIndex = -1;
  WordSelectNavigator navigator;
  DictionaryLookupController controller;

  static constexpr unsigned long LONG_PRESS_MS = 600;

  bool skipLoopDelay() override { return controller.skipLoopDelay(); }

  void wrapText();
  void wrapHtml();
  void wrapPlain();
  void extractWordsFromLayout();
  void handleNotFound(const std::string& word);
  std::string buildPhraseFromRange(int fromIdx, int toIdx) const;
};
