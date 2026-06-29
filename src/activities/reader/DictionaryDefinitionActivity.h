#pragma once
#include <DictHtmlRenderer.h>
#include <EpdFontFamily.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/DictFontUtils.h"
#include "util/DictLayout.h"
#include "util/Dictionary.h"

class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const std::string& headword, const DictLocation& location,
                                        bool showLookupButton = false, std::string bookCachePath = "")
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(headword),
        foundLocation(location),
        showLookupButton(showLookupButton),
        cachePath(std::move(bookCachePath)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string headword;
  DictLocation foundLocation;
  bool showLookupButton;
  std::string cachePath;

  struct PooledSegment {
    uint16_t offset = 0;  // into pagePool_
    uint16_t len = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    bool isDictFont = false;
  };
  struct PooledLine {
    std::vector<PooledSegment> segments;
    uint8_t indentLevel = 0;
    bool isListItem = false;
  };

  std::vector<PooledLine> layoutLines;
  std::string pagePool_;
  int currentPage = 0;
  int linesPerPage = 0;
  int totalPages = 0;

  DictHtmlRenderer htmlRenderer_;

  int collectTargetPage_ = 0;
  int collectLineCount_ = 0;

  int leftPadding = 20;
  int rightPadding = 20;
  int hintGutterHeight = 0;
  int contentX = 0;
  int hintGutterWidth = 0;
  int bodyStartY = 0;

  void wrapText();
  void loadPage(int page);
  void wrapHtml();
  void wrapPlain();
  int getMixedWidth(std::vector<DictTextSpan>& dictRuns, const char* text, EpdFontFamily::Style style);
  static int measureWidthAdapter(void* ctx, const char* text, EpdFontFamily::Style style, bool isDictFont);
  static void collectLineSink(void* ctx, DictLayout::LayoutLine&& line);
  static void feedSpanToWrapper(void* ctx, const StyledSpan& span);
  int getLineHeight() const;
};
