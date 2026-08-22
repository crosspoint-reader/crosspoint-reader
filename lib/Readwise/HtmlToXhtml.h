#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Streaming HTML-to-XHTML converter with a whitelist policy, safe across
 * arbitrary chunk boundaries. Produces well-formed XML fragments:
 * - keeps a small set of structural/inline tags (balanced automatically),
 * - drops scripts/styles/embedded media with their content,
 * - unwraps unknown tags (children pass through),
 * - resolves common named entities and numeric character references,
 * - XML-escapes text and attribute values,
 * - wraps loose text in <p> and force-closes everything at finish().
 */
class HtmlToXhtml {
 public:
  struct Sink {
    void* ctx = nullptr;
    // Return non-zero to abort feeding.
    int (*write)(void* ctx, const char* data, size_t len) = nullptr;
  };

  explicit HtmlToXhtml(const Sink& sink) : sink_(sink) {}

  void reset();
  void feed(const char* data, size_t len);
  void finish();
  bool hasError() const { return error_; }

  enum class Kind : uint8_t { PARA_BLOCK, BLOCK, INLINE, VOID_EMIT, IGNORE, DROP_CONTENT, UNWRAP, RAW };

  struct TagDef {
    const char* name;
    Kind kind;
  };

 private:
  enum class State : uint8_t {
    TEXT,
    ENTITY,          // collecting an '&...' sequence in text
    TAG_START,       // just saw '<'
    BANG,            // '<!' — deciding comment vs doctype
    COMMENT,         // inside <!-- -->
    SKIP_DECL,       // inside <!DOCTYPE ...> or <?...>
    TAG_NAME,        // collecting an open tag name
    CLOSING_NAME,    // collecting a close tag name
    TAG_ATTRS,       // inside an open tag, between attributes
    ATTR_NAME,
    ATTR_AFTER_NAME,
    ATTR_VALUE,
    RAW_TEXT         // inside <script>/<style> content
  };

  static constexpr size_t MAX_STACK = 24;
  static constexpr size_t MAX_NAME = 15;
  static constexpr size_t MAX_ENTITY = 11;
  static constexpr size_t MAX_HREF = 255;
  static constexpr size_t MAX_DROP_DEPTH = 8;

  const TagDef* findTag(const char* name) const;

  bool out(const char* s, size_t len);
  bool out(const char* s);
  bool outEscapedText(const char* s, size_t len);

  void closeTopTag();
  void closeUpTo(size_t target);
  void emitClose(const TagDef* def);
  void ensureParagraph();
  void startOpenTag(const TagDef& def, bool selfClosing);

  void handleTextChar(char c);
  void handleEntityChar(char c);
  void handleEntityEnd(bool terminated);
  void flushPendingEntityAsText(char terminator);
  void handleTagNameChar(char c);
  void handleCloseNameChar(char c);
  void handleAttrChar(char c);
  void captureAttrValue(char c);
  void completeOpenTag();
  void processCloseTag();

  Sink sink_;

  State state_ = State::TEXT;
  char tagName_[MAX_NAME + 1];
  size_t nameLen_ = 0;
  bool isClosingTag_ = false;
  bool selfClosing_ = false;

  char attrName_[15];
  size_t attrNameLen_ = 0;
  char href_[MAX_HREF + 1];
  size_t hrefLen_ = 0;
  char attrQuote_ = 0;
  bool sawHref_ = false;

  char entityBuf_[MAX_ENTITY];
  size_t entityLen_ = 0;

  char bang_[2];
  size_t bangLen_ = 0;
  size_t commentDashRun_ = 0;

  const TagDef* dropStack_[MAX_DROP_DEPTH];
  size_t dropDepth_ = 0;

  struct StackEntry {
    const TagDef* def;
  };
  StackEntry stack_[MAX_STACK];
  size_t stackSize_ = 0;
  // Stack index of the innermost open paragraph-capable block (<p>/<hN>),
  // SIZE_MAX when none. Loose text opens one lazily; any block start closes it.
  size_t paragraphIndex_ = SIZE_MAX;

  const TagDef* rawTag_ = nullptr;  // script/style whose content we're skipping
  size_t rawMatchPos_ = 0;
  bool error_ = false;
};
