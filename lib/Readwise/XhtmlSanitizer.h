#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace readwise {

// Streaming HTML -> XHTML body converter for Readwise Reader `html_content`.
//
// The chapter parser is a strict XML parser, so the output must be
// well-formed. Rather than trusting the input, every kept tag is re-emitted
// from a small allowlist with no attributes, end tags are matched against a
// stack, and text is re-escaped and UTF-8 validated. The output is therefore
// well-formed by construction whatever the input looks like.
//
// Dropped: all attributes, images and media, scripts/styles/SVG/MathML
// (with their content), comments and declarations. Unknown elements are
// unwrapped (their text is kept).
//
// Input may arrive in arbitrary chunks; state carries across feed() calls.
// Memory: one fixed object (~1.3 KB), no heap.
class XhtmlSanitizer {
 public:
  using Sink = std::function<bool(const char* data, size_t len)>;

  explicit XhtmlSanitizer(Sink sink);

  // Returns false once the sink has refused a write.
  bool feed(const char* data, size_t len);
  // Closes open elements and flushes. Call once.
  bool finish();

  // Bytes of text (not markup) emitted so far; used to detect empty articles.
  size_t textBytes() const { return textBytes_; }

 private:
  enum class State : uint8_t {
    Text,
    RawText,  // inside script/style/textarea/title: only "</name" ends it
    TagOpen,
    TagName,
    InTag,
    EndTagName,
    EndTagRest,
    Bang,
    Comment,
    Decl,
    Entity
  };

  static constexpr size_t kMaxDepth = 48;
  static constexpr size_t kNameMax = 15;
  static constexpr size_t kEntityMax = 12;
  static constexpr size_t kOutBuf = 1024;

  void step(char c);
  void onStartTag(bool selfClosing);
  void onEndTag();
  void flushEntity(bool terminatedBySemicolon);
  void textByte(uint8_t b);
  void emitCodepoint(uint32_t cp);
  void emitInvalid();
  void out(const char* s, size_t n);
  void out(const char* s);
  bool flush();

  Sink sink_;
  bool ok_ = true;
  State state_ = State::Text;

  char name_[kNameMax + 1] = {};
  uint8_t nameLen_ = 0;
  bool nameOverflow_ = false;
  char quote_ = 0;
  char lastNonSpace_ = 0;
  uint8_t bangLen_ = 0;  // chars seen after "<!" (to spot "<!--")
  uint8_t dashRun_ = 0;  // consecutive '-' inside a comment

  char entity_[kEntityMax + 1] = {};
  uint8_t entityLen_ = 0;

  // Open kept elements (indices into the allowlist).
  uint8_t stack_[kMaxDepth] = {};
  uint8_t depth_ = 0;
  // Inside a dropped subtree (script, svg, ...): its allowlist-independent
  // name and nesting count.
  int8_t skipTag_ = -1;
  uint8_t skipNest_ = 0;
  uint8_t rawMatch_ = 0;  // chars of "</name" matched in RawText

  // UTF-8 validation of text.
  uint32_t cp_ = 0;
  uint8_t need_ = 0;
  uint8_t seqLen_ = 0;

  char buf_[kOutBuf];
  size_t bufLen_ = 0;
  size_t textBytes_ = 0;
  bool lastWasSpace_ = true;
};

}  // namespace readwise
