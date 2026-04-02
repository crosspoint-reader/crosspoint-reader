#pragma once

#include <expat.h>

#include <cstdint>
#include <string>
#include <vector>

/**
 * A styled text span produced by DictHtmlRenderer.
 */
struct StyledSpan {
  const char* text = nullptr;  // Points into renderer-owned buffer; valid until next render() call
  uint32_t textOffset = 0;     // Internal: byte offset into textBuf; converted to text after render()
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

  // Stream-render from an open file. Reads definition in 512-byte chunks — the full
  // definition is never held in RAM. Entity resolution handles cross-chunk boundaries.
  // The file must be open for reading; caller is responsible for close.
  const std::vector<StyledSpan>& renderFromFile(const char* dictPath, uint32_t offset, uint32_t size);

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
    std::string abbrTitle;
    std::string wikiText;
#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
    // For unknown tag tracking: tag name recorded at open
    char unknownTagName[48];
    // Pending text length at the time unknown tag opened (to extract contents later)
    int pendingLenAtOpen;
#endif
  };

  void reset();
  void pushSpan();
  void emitText(const char* s, int len);
  void flushPending();

  // Feed a buffer through the entity resolver into expat. If the buffer ends mid-entity,
  // the partial entity is written to carry/carryLen for prepending to the next chunk.
  // Set isLast=true for the final chunk (no carryover possible).
  void feedEntityResolved(const char* buf, int len, bool isLast, char* carry, int* carryLen);

  std::vector<StackEntry> tagStack;

  FormatState fmt;

  // Transient rendering flags — NOT saved/restored per tag
  bool newlinePending = false;
  bool listItemPending = false;

  std::vector<char> textBuf;

  std::string pendingText;

  std::vector<StyledSpan> spans;

  XML_Parser parser = nullptr;
  bool parseError = false;

#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
  static void extractLastWord(const char* text, int len, char* out, int outSize);
  void recordUnknownTag(const char* tagName, const char* wordBefore, const char* tagContents, const char* wordAfter);
#endif
};
