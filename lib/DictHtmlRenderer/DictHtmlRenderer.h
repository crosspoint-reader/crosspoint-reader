#pragma once

#include <expat.h>

#include <cstdint>
#include <vector>

/**
 * A styled text span produced by DictHtmlRenderer.
 */
struct StyledSpan {
  const char* text = nullptr;  // Points into renderer-owned buffer; valid until next render() call
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool strikethrough = false;
  bool isListItem = false;
  bool newlineBefore = false;
  uint8_t indentLevel = 0;
};

/**
 * Renders StarDict HTML definitions (sametypesequence=h) into a flat vector
 * of StyledSpan objects suitable for display on e-ink.
 *
 * Usage:
 *   DictHtmlRenderer renderer;
 *   const std::vector<StyledSpan>& spans = renderer.render(html, len);
 *
 * The returned reference is valid until the next call to render().
 * The renderer is reusable.
 */
class DictHtmlRenderer {
 public:
  DictHtmlRenderer();
  ~DictHtmlRenderer();

  DictHtmlRenderer(const DictHtmlRenderer&) = delete;
  DictHtmlRenderer& operator=(const DictHtmlRenderer&) = delete;

  const std::vector<StyledSpan>& render(const char* html, int len);

#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
  bool hasUnknownTags() const { return unknownTagCount > 0; }

  struct UnknownTagInfo {
    char tag[48];
    char entry[48];
    char wordBefore[32];
    char tagContents[64];
    char wordAfter[32];
  };

  static constexpr int MAX_UNKNOWN_TAGS = 16;
  UnknownTagInfo unknownTags[MAX_UNKNOWN_TAGS];
  int unknownTagCount = 0;

  const char* currentEntryName = nullptr;
#endif

 private:
  static void XMLCALL onStart(void* ud, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL onEnd(void* ud, const XML_Char* name);
  static void XMLCALL onText(void* ud, const XML_Char* s, int len);
  static const char* findAttr(const XML_Char** atts, const char* name);

  enum class TagAction {
    BLOCK_STRIP,
    FORMAT_BOLD,
    FORMAT_ITALIC,
    FORMAT_UNDERLINE,
    FORMAT_STRIKE,
    FORMAT_SMALL,
    FORMAT_CODE,
    BLOCK_BREAK,
    BLOCK_QUOTE,
    LIST_ITEM,
    LIST_CONTAINER,
    HEADING,
    ABBR,
    VAR,
    SPAN,
    WIKI_ANNOT,
    REGISTERED,
    STRIP_KEEP,
  };

  static TagAction classify(const XML_Char* name);

  // Format state that is saved/restored at tag boundaries.
  // Does NOT include transient flags (newlinePending, listItemPending).
  struct FormatState {
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
    uint8_t indentLevel = 0;
  };

  struct StackEntry {
    TagAction action;
    FormatState savedFmt;
    bool suppressChildren;
    char abbrTitle[64];
    bool hasAbbrTitle;
    char wikiText[64];
    bool hasWikiText;
    // For unknown tag tracking: tag name recorded at open
    char unknownTagName[48];
    // Pending text length at the time unknown tag opened (to extract contents later)
    int pendingLenAtOpen;
  };

  void reset();
  void pushSpan();
  void emitText(const char* s, int len);
  void flushPending();

  static constexpr int MAX_STACK = 32;
  StackEntry stack[MAX_STACK];
  int stackDepth = 0;

  FormatState fmt;

  // Transient rendering flags — NOT saved/restored per tag
  bool newlinePending = false;
  bool listItemPending = false;

  static constexpr int TEXT_BUF_SIZE = 8192;
  char textBuf[TEXT_BUF_SIZE];
  int textBufPos = 0;

  static constexpr int PENDING_SIZE = 512;
  char pendingText[PENDING_SIZE];
  int pendingLen = 0;

  std::vector<StyledSpan> spans;

  XML_Parser parser = nullptr;
  bool parseError = false;

#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
  static void extractLastWord(const char* text, int len, char* out, int outSize);
  void recordUnknownTag(const char* tagName, const char* wordBefore, const char* tagContents, const char* wordAfter);
#endif
};
