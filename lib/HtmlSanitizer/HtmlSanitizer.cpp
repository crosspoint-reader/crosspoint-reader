#include "HtmlSanitizer.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <strings.h>

#include <algorithm>
#include <cstdint>

namespace HtmlSanitizer {

namespace {

// HTML5 void elements that must be self-closed for expat XML compatibility.
constexpr const char* VOID_TAGS[] = {"br",   "hr",  "img",   "input", "link",   "meta",  "area",
                                     "base", "col", "embed", "param", "source", "track", "wbr"};

bool isVoidTag(const char* name) {
  for (const char* vt : VOID_TAGS) {
    if (strcasecmp(name, vt) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool selfCloseVoidElements(const std::string& src, const std::string& dst, bool& modified) {
  static constexpr size_t CHUNK = 512;
  // Bound on buffered raw bytes for a not-yet-classified end tag ("</" + name + a little
  // slack). Void tag names are at most 6 chars ("source"/"track"), so 15 chars of headroom
  // is generous while keeping the buffer small.
  static constexpr size_t END_TAG_RAW_CAP = 18;

  modified = false;

  // Heap-allocate I/O buffers to stay within 256-byte stack limit
  auto inBuf = makeUniqueNoThrow<uint8_t[]>(CHUNK);
  auto outBuf = makeUniqueNoThrow<uint8_t[]>(CHUNK);
  if (!inBuf || !outBuf) {
    LOG_ERR("HSAN", "selfCloseVoidElements: OOM: %d bytes", static_cast<int>(CHUNK));
    return false;
  }

  HalFile inFile, outFile;
  if (!Storage.openFileForRead("HSAN", src, inFile)) {
    return false;
  }
  if (!Storage.openFileForWrite("HSAN", dst, outFile)) {
    return false;
  }

  // State machine:
  //   NORMAL              - streaming text outside any tag
  //   TAG_OPEN            - just saw '<', deciding start tag / end tag / '!' / '?' construct
  //   IN_START_TAG        - streaming a start tag; tracks quotes and inserts '/' before '>'
  //                         for unclosed void elements
  //   END_TAG_NAME        - buffering an end tag's name to classify it before writing anything
  //   END_TAG_TRAILING_WS - end tag name matched a void element; consuming trailing whitespace
  //                         before the closing '>' so the whole tag can be dropped
  //   BANG_SNIFF          - just saw '<!'; one more byte decides comment / CDATA / other decl
  //   BANG_DASH           - saw '<!-'; one more '-' confirms a comment, otherwise a plain decl
  //   COMMENT             - inside <!-- ... -->; scans for the "-->" terminator, quotes ignored
  //   CDATA               - inside <![CDATA[ ... ]]>; scans for the "]]>" terminator (any
  //                         "<![" is treated as CDATA rather than spelling-matching "CDATA[" —
  //                         cheap and correct for real EPUB content, where "<![" never appears
  //                         for anything else)
  //   PASSTHROUGH_UNTIL_GT - echoes bytes verbatim until the next '>', with no quote tracking
  //                         and no modification; used only for non-void end tags and the
  //                         fail-open paths in END_TAG_NAME / END_TAG_TRAILING_WS. End tags
  //                         cannot legally contain a bare '>', so terminating on the first one
  //                         is correct there. NOT used for PIs or '<!' declarations — see
  //                         PI_UNTIL_QGT and DECL_UNTIL_GT below, which those need instead
  //                         because a bare '>' can legally appear inside their content.
  //   PI_UNTIL_QGT        - inside a processing instruction (<?xml ... ?>, <?xml-stylesheet
  //                         ... ?>); echoes bytes verbatim and terminates only on the two-byte
  //                         "?>" sequence, so a bare '>' in the PI body (e.g. an attribute
  //                         value) does not end it early
  //   DECL_UNTIL_GT       - inside a '<!' declaration other than a comment/CDATA (DOCTYPE and
  //                         similar); echoes bytes verbatim, tracks quoted literals so a '>'
  //                         inside a quote doesn't terminate, and tracks internal-subset
  //                         bracket depth ('[' / ']', e.g. DOCTYPE's "[ <!ENTITY ...> ]") so a
  //                         '>' inside the subset doesn't terminate either; only an unquoted
  //                         '>' at subset depth 0 ends the declaration
  enum class State : uint8_t {
    NORMAL,
    TAG_OPEN,
    IN_START_TAG,
    END_TAG_NAME,
    END_TAG_TRAILING_WS,
    BANG_SNIFF,
    BANG_DASH,
    COMMENT,
    CDATA,
    PASSTHROUGH_UNTIL_GT,
    PI_UNTIL_QGT,
    DECL_UNTIL_GT
  };
  State state = State::NORMAL;

  char tagName[16] = {};
  uint8_t tagNameLen = 0;
  bool inVoidTag = false;
  bool pastTagName = false;  // true once the tag name has been fully read
  char lastNonSpaceChar = 0;
  char quoteChar = 0;               // non-zero while inside a quoted attribute value or DECL literal
  uint8_t terminatorRunLength = 0;  // trailing count of '-' (COMMENT) or ']' (CDATA) seen, or
                                    // "saw '?'" flag (0/1) for PI_UNTIL_QGT
  uint8_t declSubsetDepth = 0;      // DECL_UNTIL_GT: nesting depth of an unquoted '[' ... ']'
                                    // internal subset (e.g. DOCTYPE's "[ <!ENTITY ...> ]")

  char endTagRaw[END_TAG_RAW_CAP] = {};
  size_t endTagRawLen = 0;

  size_t outLen = 0;
  bool ioError = false;

  auto flushOut = [&]() {
    if (outLen > 0) {
      const size_t written = outFile.write(outBuf.get(), outLen);
      if (written != outLen) {
        ioError = true;
      }
      outLen = 0;
    }
  };

  auto writeChar = [&](char c) {
    if (ioError) {
      return;
    }
    outBuf[outLen++] = static_cast<uint8_t>(c);
    if (outLen >= CHUNK) {
      flushOut();
    }
  };

  // Flushes the buffered end-tag bytes verbatim (used when the tag turns out not to be a
  // droppable void end tag).
  auto flushEndTagRaw = [&]() {
    for (size_t i = 0; i < endTagRawLen; i++) {
      writeChar(endTagRaw[i]);
    }
    endTagRawLen = 0;
  };

  auto bufferEndTagRaw = [&](char c) {
    if (endTagRawLen < END_TAG_RAW_CAP) {
      endTagRaw[endTagRawLen++] = c;
    } else {
      // Buffer exhausted before classification — cannot be a void tag name; fail open.
      flushEndTagRaw();
      writeChar(c);
      state = State::PASSTHROUGH_UNTIL_GT;
    }
  };

  // Processes one character of a start tag, tracking quoted attribute values so a literal
  // '>' inside a quote does not end the tag, and so a '/' inside a quote is never mistaken
  // for an existing self-close. Comments, CDATA, PIs, and other '<!' declarations never
  // reach this lambda — TAG_OPEN routes them to dedicated quote-agnostic states instead.
  auto handleStartTagChar = [&](char c) {
    if (quoteChar != 0) {
      writeChar(c);
      if (c == quoteChar) {
        quoteChar = 0;
        lastNonSpaceChar = c;
      }
      return;
    }

    if (c == '"' || c == '\'') {
      quoteChar = c;
      writeChar(c);
      lastNonSpaceChar = c;
      return;
    }

    if (c == '>') {
      if (!pastTagName && tagNameLen > 0) {
        tagName[tagNameLen] = '\0';
        inVoidTag = isVoidTag(tagName);
      }
      if (inVoidTag && lastNonSpaceChar != '/') {
        writeChar('/');
        modified = true;
      }
      writeChar('>');
      state = State::NORMAL;
      return;
    }

    writeChar(c);
    if (!pastTagName) {
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
        pastTagName = true;
        tagName[tagNameLen] = '\0';
        inVoidTag = isVoidTag(tagName);
      } else if (tagNameLen < 15) {
        tagName[tagNameLen++] = c;
      }
    }
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
      lastNonSpaceChar = c;
    }
  };

  while (inFile.available() && !ioError) {
    const int bytesRead = inFile.read(inBuf.get(), CHUNK);
    if (bytesRead <= 0) {
      LOG_ERR("HSAN", "selfCloseVoidElements: read failed on %s", src.c_str());
      ioError = true;
      break;
    }
    const size_t read = static_cast<size_t>(bytesRead);
    for (size_t i = 0; i < read && !ioError; i++) {
      const char c = static_cast<char>(inBuf[i]);

      switch (state) {
        case State::NORMAL:
          if (c == '<') {
            state = State::TAG_OPEN;
          } else {
            writeChar(c);
          }
          break;

        case State::TAG_OPEN:
          if (c == '/') {
            state = State::END_TAG_NAME;
            tagNameLen = 0;
            endTagRawLen = 0;
            endTagRaw[endTagRawLen++] = '<';
            endTagRaw[endTagRawLen++] = '/';
          } else if (c == '!') {
            writeChar('<');
            writeChar('!');
            state = State::BANG_SNIFF;
          } else if (c == '?') {
            writeChar('<');
            writeChar('?');
            terminatorRunLength = 0;
            state = State::PI_UNTIL_QGT;
          } else {
            writeChar('<');
            tagNameLen = 0;
            tagName[0] = '\0';
            inVoidTag = false;
            pastTagName = false;
            lastNonSpaceChar = 0;
            quoteChar = 0;
            state = State::IN_START_TAG;
            handleStartTagChar(c);
          }
          break;

        case State::IN_START_TAG:
          handleStartTagChar(c);
          break;

        case State::END_TAG_NAME:
          if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            bufferEndTagRaw(c);
            if (state == State::END_TAG_NAME) {
              tagName[tagNameLen] = '\0';
              if (isVoidTag(tagName)) {
                state = State::END_TAG_TRAILING_WS;
              } else {
                flushEndTagRaw();
                state = State::PASSTHROUGH_UNTIL_GT;
              }
            }
          } else if (c == '>') {
            tagName[tagNameLen] = '\0';
            if (tagNameLen > 0 && isVoidTag(tagName)) {
              endTagRawLen = 0;  // drop the whole end tag
              modified = true;
              state = State::NORMAL;
            } else {
              bufferEndTagRaw(c);
              if (state == State::END_TAG_NAME) {
                flushEndTagRaw();
                state = State::NORMAL;
              }
            }
          } else if (tagNameLen < 15) {
            tagName[tagNameLen++] = c;
            bufferEndTagRaw(c);
          } else {
            // Name longer than any void tag — cannot match; fail open.
            flushEndTagRaw();
            writeChar(c);
            state = State::PASSTHROUGH_UNTIL_GT;
          }
          break;

        case State::END_TAG_TRAILING_WS:
          if (c == '>') {
            endTagRawLen = 0;  // drop the whole end tag
            modified = true;
            state = State::NORMAL;
          } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            bufferEndTagRaw(c);
          } else {
            // Unexpected content after the name (e.g. an attribute) — not a plain void
            // end tag; fail open and emit everything buffered so far verbatim.
            bufferEndTagRaw(c);
            if (state == State::END_TAG_TRAILING_WS) {
              flushEndTagRaw();
              state = State::PASSTHROUGH_UNTIL_GT;
            }
          }
          break;

        case State::BANG_SNIFF:
          writeChar(c);
          if (c == '-') {
            state = State::BANG_DASH;
          } else if (c == '[') {
            state = State::CDATA;
            terminatorRunLength = 0;
          } else if (c == '>') {
            state = State::NORMAL;
          } else {
            quoteChar = 0;
            declSubsetDepth = 0;
            state = State::DECL_UNTIL_GT;
          }
          break;

        case State::BANG_DASH:
          writeChar(c);
          if (c == '-') {
            state = State::COMMENT;
            terminatorRunLength = 0;
          } else if (c == '>') {
            state = State::NORMAL;
          } else {
            quoteChar = 0;
            declSubsetDepth = 0;
            state = State::DECL_UNTIL_GT;
          }
          break;

        case State::COMMENT:
          // Scans for "-->" without any quote tracking — comment bodies are opaque text,
          // so a "don't"-style apostrophe must never open a quoted region.
          writeChar(c);
          if (c == '-') {
            terminatorRunLength = std::min<uint8_t>(terminatorRunLength + 1, 2);
          } else if (c == '>' && terminatorRunLength >= 2) {
            state = State::NORMAL;
            terminatorRunLength = 0;
          } else {
            terminatorRunLength = 0;
          }
          break;

        case State::CDATA:
          // Scans for "]]>"; a raw '<' or '>' inside the section is legal CDATA content and
          // must not be reinterpreted as a tag boundary.
          writeChar(c);
          if (c == ']') {
            terminatorRunLength = std::min<uint8_t>(terminatorRunLength + 1, 2);
          } else if (c == '>' && terminatorRunLength >= 2) {
            state = State::NORMAL;
            terminatorRunLength = 0;
          } else {
            terminatorRunLength = 0;
          }
          break;

        case State::PASSTHROUGH_UNTIL_GT:
          writeChar(c);
          if (c == '>') {
            state = State::NORMAL;
          }
          break;

        case State::PI_UNTIL_QGT:
          // Verbatim copy; only the two-byte "?>" sequence terminates. A lone '>' (e.g.
          // inside an unquoted attribute value in the PI body) must not end it early.
          writeChar(c);
          if (c == '?') {
            terminatorRunLength = 1;
          } else if (c == '>' && terminatorRunLength >= 1) {
            state = State::NORMAL;
            terminatorRunLength = 0;
          } else {
            terminatorRunLength = 0;
          }
          break;

        case State::DECL_UNTIL_GT:
          // Verbatim copy; tracks quoted literals and internal-subset '[' ']' bracket depth
          // so a '>' inside either does not end the declaration early.
          writeChar(c);
          if (quoteChar != 0) {
            if (c == quoteChar) {
              quoteChar = 0;
            }
          } else if (c == '"' || c == '\'') {
            quoteChar = c;
          } else if (c == '[') {
            declSubsetDepth++;
          } else if (c == ']') {
            if (declSubsetDepth > 0) {
              declSubsetDepth--;
            }
          } else if (c == '>' && declSubsetDepth == 0) {
            state = State::NORMAL;
          }
          break;
      }
    }
  }

  // File ended mid-tag (truncated/malformed input) — flush whatever was buffered rather
  // than silently dropping it.
  if (!ioError) {
    if (state == State::TAG_OPEN) {
      writeChar('<');
    } else if (state == State::END_TAG_NAME || state == State::END_TAG_TRAILING_WS) {
      flushEndTagRaw();
    }
  }

  flushOut();
  inFile.close();
  // Explicitly close outFile so the sanitized copy is flushed before the caller reopens it
  const bool closedOk = outFile.close();
  if (ioError || !closedOk) {
    LOG_ERR("HSAN", "selfCloseVoidElements: I/O error producing %s", dst.c_str());
    return false;
  }
  return true;
}

}  // namespace HtmlSanitizer
