#pragma once
#include <EpdFontFamily.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/DictLayout.h"
#include "util/DictionaryLookupController.h"
#include "util/IpaUtils.h"
#include "util/LookupChain.h"
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
  // Cross-definition back-navigation stack (compact: history-index + page per
  // entry, not owned strings). pendingBack_ carries the popped entry from the
  // Back keypress to the async FoundDefinition that completes the re-lookup.
  LookupChain chain_;
  LookupChain::Entry pendingBack_{};
  bool chainBackNavInProgress = false;

  // Resident page representation (Stage 2b-pool). Segments reference text by
  // {offset, len} into pagePool_ instead of owning a std::string each — the
  // Wrapper already merged same-style runs, so each segment is one pooled,
  // null-terminated entry (kerning preserved, valid for C-API drawText).
  struct PooledSegment {
    uint16_t offset = 0;  // into pagePool_
    uint16_t len = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    bool isIpa = false;
  };
  struct PooledLine {
    std::vector<PooledSegment> segments;
    uint8_t indentLevel = 0;
    bool isListItem = false;
  };

  // layoutLines holds ONLY the current page's lines (Stage 2a streaming). render
  // and extractWordsFromLayout index it from 0; loadPage() refills it per turn.
  // pagePool_ backs all segment text for the resident page.
  std::vector<PooledLine> layoutLines;
  std::string pagePool_;
  int currentPage = 0;
  int linesPerPage = 0;
  int totalPages = 0;

  // Reused across page turns (3.1-A): avoids re-allocating the renderer object +
  // its parser/buffers on every loadPage. A value member is fine — the activity
  // is heap-allocated, so this lives on the heap. reset()+re-feed each turn (NOT
  // kept alive mid-parse; that is the won't-fixed 2c).
  DictHtmlRenderer htmlRenderer_;

  // Page-collector state (used by collectLineSink during a wrap pass): keep only
  // collectTargetPage_'s lines into layoutLines, counting all lines produced.
  int collectTargetPage_ = 0;
  int collectLineCount_ = 0;

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
  // Re-parse the definition and lay out ONLY page `page` into layoutLines,
  // discarding other pages as they are produced; also recomputes totalPages.
  void loadPage(int page);
  void wrapHtml();
  void wrapPlain();
  void extractWordsFromLayout();
  int getMixedWidth(std::vector<IpaTextSpan>& ipaRuns, const char* text, EpdFontFamily::Style style);
  // Width measurement adapter injected into DictLayout::wrapSpans. ctx is `this`.
  static int measureWidthAdapter(void* ctx, const char* text, EpdFontFamily::Style style, bool isIpa);
  // Line sink injected into DictLayout::wrapSpans: keeps collectTargetPage_'s
  // lines, counts the rest. ctx is `this`.
  static void collectLineSink(void* ctx, DictLayout::LayoutLine&& line);
  // Span sink bridge: forwards each streamed span from DictHtmlRenderer into the
  // DictLayout::Wrapper. ctx is the Wrapper*.
  static void feedSpanToWrapper(void* ctx, const StyledSpan& span);
  bool handleLongPressExitAll(bool enabled);
  int getLineHeight() const;
};
