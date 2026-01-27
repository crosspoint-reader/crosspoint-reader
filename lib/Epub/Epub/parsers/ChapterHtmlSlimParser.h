#pragma once

#include <expat.h>

#include <climits>
#include <cstring>
#include <functional>
#include <memory>

#include "../FootnoteEntry.h"
#include "../ParsedText.h"
#include "../blocks/TextBlock.h"

class Page;
class GfxRenderer;

#define MAX_WORD_SIZE 200

struct Noteref {
  char number[16];
  char href[128];
};

// Struct to store collected inline footnotes (aside elements)
struct InlineFootnote {
  char id[3];
  char* text;

  InlineFootnote() : text(nullptr) { id[0] = '\0'; }
};

// Struct to store collected inline footnotes from <p class="note">
struct ParagraphNote {
  char id[16];  // ID from <a id="rnote1">
  char* text;   // Pointer to dynamically allocated text

  ParagraphNote() : text(nullptr) { id[0] = '\0'; }

  ~ParagraphNote() {
    if (text) {
      free(text);
      text = nullptr;
    }
  }

  ParagraphNote(const ParagraphNote&) = delete;
  ParagraphNote& operator=(const ParagraphNote&) = delete;
};

class ChapterHtmlSlimParser {
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>)> completePageFn;
  std::function<void(int)> progressFn;  // Progress callback (0-100)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;

  // Noteref tracking
  bool insideNoteref = false;
  char currentNoterefText[16] = {0};
  int currentNoterefTextLen = 0;
  char currentNoterefHref[128] = {0};
  int currentNoterefHrefLen = 0;
  std::function<void(Noteref&)> noterefCallback = nullptr;

  // Footnote tracking for current page
  FootnoteEntry currentPageFootnotes[16];
  int currentPageFootnoteCount = 0;

  // Inline footnotes (aside) tracking
  bool insideAsideFootnote = false;
  int asideDepth = 0;
  char currentAsideId[3] = {0};

  // Paragraph note tracking
  bool insideParagraphNote = false;
  int paragraphNoteDepth = 0;
  char currentParagraphNoteId[16] = {0};
  static constexpr int MAX_PNOTE_BUFFER = 256;
  char currentParagraphNoteText[MAX_PNOTE_BUFFER] = {0};
  int currentParagraphNoteTextLen = 0;

  // Temporary buffer for accumulation, will be copied to dynamic allocation
  static constexpr int MAX_ASIDE_BUFFER = 1024;
  char currentAsideText[MAX_ASIDE_BUFFER] = {0};
  int currentAsideTextLen = 0;

  // Flag to indicate we're in Pass 1 (collecting asides only)
  bool isPass1CollectingAsides = false;

  // Track superscript depth
  int supDepth = -1;
  int anchorDepth = -1;

  void addFootnoteToCurrentPage(const char* number, const char* href);
  void startNewTextBlock(TextBlock::Style style);
  EpdFontFamily::Style getCurrentFontStyle() const;
  void flushPartWordBuffer();
  void makePages();

  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  // inline footnotes
  InlineFootnote inlineFootnotes[16];
  int inlineFootnoteCount = 0;
  // paragraph notes
  ParagraphNote paragraphNotes[16];
  int paragraphNoteCount = 0;

  explicit ChapterHtmlSlimParser(const std::string& filepath, GfxRenderer& renderer, const int fontId,
                                 const float lineCompression, const bool extraParagraphSpacing,
                                 const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                 const uint16_t viewportHeight, const bool hyphenationEnabled,
                                 const std::function<void(std::unique_ptr<Page>)>& completePageFn,
                                 const std::function<void(int)>& progressFn = nullptr)
      : filepath(filepath),
        renderer(renderer),
        completePageFn(completePageFn),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        progressFn(progressFn),
        inlineFootnoteCount(0) {
    // Initialize all footnote pointers to null
    for (int i = 0; i < 16; i++) {
      inlineFootnotes[i].text = nullptr;
      inlineFootnotes[i].id[0] = '\0';
    }
  }

  ~ChapterHtmlSlimParser() {
    // Manual cleanup of inline footnotes
    for (int i = 0; i < inlineFootnoteCount; i++) {
      if (inlineFootnotes[i].text) {
        free(inlineFootnotes[i].text);
        inlineFootnotes[i].text = nullptr;
      }
    }
  }

  bool parseAndBuildPages();
  void addLineToPage(std::shared_ptr<TextBlock> line);

  void setNoterefCallback(const std::function<void(Noteref&)>& callback) { noterefCallback = callback; }
};
