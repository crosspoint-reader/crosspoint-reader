#include "ReadwiseJsonParser.h"

#include <cstring>

namespace {
int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return c - 'A' + 10;
}

size_t encodeUtf8(uint32_t cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

// Captured keys, matched against the raw JSON key text.
constexpr const char* DOC_KEYS[] = {"id", "title", "author", "html_content"};
constexpr const char* ROOT_KEYS[] = {"nextPageCursor"};
}  // namespace

void ReadwiseJsonParser::reset() {
  state_ = State::SCANNING;
  depth_ = 0;
  expectingValue_ = false;
  keyLen_ = 0;
  keyOverflow_ = false;
  resultsArrayDepth_ = -1;
  docObjectDepth_ = -1;
  outLen_ = 0;
  unicode_ = UnicodeState::NONE;
  pendingSurrogate_ = 0;
  literalLen_ = 0;
  escaped_ = false;
  error_ = false;
}

void ReadwiseJsonParser::pushContainer(const Container c) {
  if (depth_ < MAX_DEPTH) {
    stack_[depth_] = c;
  }
  ++depth_;
}

void ReadwiseJsonParser::popContainer() {
  if (depth_ > 0) --depth_;
  // Document ends only when we pop the object itself (its level minus one),
  // not when a nested array/object closes back into it.
  if (docObjectDepth_ >= 0 && static_cast<int>(depth_) == docObjectDepth_ - 1) {
    if (cb.onDocumentEnd) cb.onDocumentEnd(cb.ctx);
    docObjectDepth_ = -1;
  }
  if (resultsArrayDepth_ >= 0 && static_cast<int>(depth_) < resultsArrayDepth_) {
    resultsArrayDepth_ = -1;
  }
}

bool ReadwiseJsonParser::captureCurrentString() const {
  if (keyOverflow_ || !cb.onField || depth_ > MAX_DEPTH) return false;
  const int currentDepth = static_cast<int>(depth_);
  // Document fields: direct string children of a document object.
  if (resultsArrayDepth_ >= 0 && currentDepth == docObjectDepth_) {
    for (const char* k : DOC_KEYS) {
      if (strcmp(keyBuf_, k) == 0) return true;
    }
  }
  // Root-level fields (nextPageCursor).
  if (currentDepth == 1) {
    for (const char* k : ROOT_KEYS) {
      if (strcmp(keyBuf_, k) == 0) return true;
    }
  }
  return false;
}

void ReadwiseJsonParser::emitDecoded(const char* bytes, const size_t len) {
  if (!captureCurrentString()) return;
  if (cb.onField(cb.ctx, keyBuf_, bytes, len, /*finalChunk=*/false) != 0) {
    error_ = true;
  }
}

void ReadwiseJsonParser::flushOutput(const bool finalChunk) {
  if (outLen_ > 0) {
    emitDecoded(outBuf_, outLen_);
    outLen_ = 0;
  }
  if (finalChunk && captureCurrentString()) {
    // Signal completion with a zero-length final chunk.
    cb.onField(cb.ctx, keyBuf_, "", 0, true);
  }
}

bool ReadwiseJsonParser::appendDecoded(const uint8_t byte) {
  if (outLen_ >= OUT_BUF_SIZE) {
    flushOutput(false);
    if (error_) return false;
  }
  outBuf_[outLen_++] = static_cast<char>(byte);
  return true;
}

void ReadwiseJsonParser::decodeUnicodeEscape() {
  uint32_t cp = 0;
  for (const char h : hexBuf_) cp = (cp << 4) | static_cast<uint32_t>(hexValue(h));

  if (unicode_ == UnicodeState::SURROGATE_HEX1) {
    unicode_ = UnicodeState::NONE;
    if (cp >= 0xDC00 && cp <= 0xDFFF) {
      const uint32_t combined = 0x10000 + ((static_cast<uint32_t>(pendingSurrogate_) - 0xD800) << 10) + (cp - 0xDC00);
      char utf8[4];
      const size_t n = encodeUtf8(combined, utf8);
      for (size_t i = 0; i < n; i++) appendDecoded(static_cast<uint8_t>(utf8[i]));
    }
    // Invalid low surrogate: drop silently rather than corrupting output.
    return;
  }

  if (cp >= 0xD800 && cp <= 0xDBFF) {
    // High surrogate: expect "\uXXXX" next.
    pendingSurrogate_ = static_cast<uint16_t>(cp);
    unicode_ = UnicodeState::SURROGATE_HEX1;
    return;
  }

  unicode_ = UnicodeState::NONE;
  char utf8[4];
  const size_t n = encodeUtf8(cp, utf8);
  for (size_t i = 0; i < n; i++) appendDecoded(static_cast<uint8_t>(utf8[i]));
}

void ReadwiseJsonParser::feed(const char* data, const size_t len) {
  for (size_t i = 0; i < len && !error_; ++i) {
    switch (state_) {
      case State::SCANNING:
        handleScanning(data[i]);
        break;
      case State::IN_KEY:
        handleKeyChar(data[i]);
        break;
      case State::IN_STRING:
        handleStringChar(data[i]);
        break;
      case State::IN_SKIP_STRING:
        if (escaped_) {
          escaped_ = false;
        } else if (data[i] == '\\') {
          escaped_ = true;
        } else if (data[i] == '"') {
          state_ = State::SCANNING;
          expectingValue_ = false;
        }
        break;
      case State::IN_NUMBER:
        handleNumber(data[i]);
        break;
      case State::IN_LITERAL:
        handleLiteral(data[i]);
        break;
    }
  }
}

void ReadwiseJsonParser::handleScanning(const char c) {
  switch (c) {
    case '"':
      if (expectingValue_ || (depth_ > 0 && stack_[depth_ - 1] == Container::ARRAY)) {
        // String value: captured when the last key is interesting, skipped otherwise.
        if (captureCurrentString()) {
          state_ = State::IN_STRING;
        } else {
          state_ = State::IN_SKIP_STRING;
          escaped_ = false;
        }
      } else {
        keyLen_ = 0;
        keyOverflow_ = false;
        state_ = State::IN_KEY;
      }
      expectingValue_ = false;
      break;
    case '{':
      pushContainer(Container::OBJECT);
      expectingValue_ = false;
      // Entering a document object: direct element of the results array.
      if (resultsArrayDepth_ >= 0 && static_cast<int>(depth_) == resultsArrayDepth_ + 1) {
        docObjectDepth_ = static_cast<int>(depth_);
        if (cb.onDocumentStart) cb.onDocumentStart(cb.ctx);
      }
      break;
    case '}':
      popContainer();
      expectingValue_ = false;
      break;
    case '[':
      pushContainer(Container::ARRAY);
      if (!keyOverflow_ && strcmp(keyBuf_, "results") == 0) {
        resultsArrayDepth_ = static_cast<int>(depth_);
      }
      keyLen_ = 0;
      keyOverflow_ = false;
      expectingValue_ = false;
      break;
    case ']':
      popContainer();
      expectingValue_ = false;
      break;
    case ':':
      expectingValue_ = true;
      break;
    case ',':
      expectingValue_ = false;
      keyLen_ = 0;
      break;
    default:
      if (expectingValue_) {
        if (c == '-' || (c >= '0' && c <= '9')) {
          state_ = State::IN_NUMBER;
        } else if (c == 't' || c == 'f' || c == 'n') {
          state_ = State::IN_LITERAL;
          literalLen_ = 1;
        }
      }
      break;
  }
}

void ReadwiseJsonParser::handleKeyChar(const char c) {
  if (escaped_) {
    escaped_ = false;
    if (c == '"') {
      state_ = State::SCANNING;
      keyBuf_[keyLen_] = '\0';
      return;
    }
    // Escapes other than \" never appear in our ASCII keys; mark overflow so
    // the value can never be mis-attributed.
    keyOverflow_ = true;
    return;
  }
  if (c == '\\') {
    escaped_ = true;
    return;
  }
  if (c == '"') {
    state_ = State::SCANNING;
    keyBuf_[keyLen_] = '\0';
    return;
  }
  if (keyLen_ < KEY_BUF_SIZE - 1) {
    keyBuf_[keyLen_++] = c;
  } else {
    keyOverflow_ = true;
  }
}

void ReadwiseJsonParser::handleStringChar(const char c) {
  if (escaped_) {
    escaped_ = false;
    switch (c) {
      case '"':
      case '\\':
      case '/':
        appendDecoded(static_cast<uint8_t>(c));
        break;
      case 'b':
        appendDecoded('\b');
        break;
      case 'f':
        appendDecoded('\f');
        break;
      case 'n':
        appendDecoded('\n');
        break;
      case 'r':
        appendDecoded('\r');
        break;
      case 't':
        appendDecoded('\t');
        break;
      case 'u':
        unicode_ = UnicodeState::HEX1;
        hexLen_ = 0;
        break;
      default:
        break;
    }
    return;
  }

  if (c == '\\') {
    escaped_ = true;
    return;
  }

  if (c == '"') {
    flushOutput(true);
    state_ = State::SCANNING;
    expectingValue_ = false;
    return;
  }

  if (unicode_ != UnicodeState::NONE) {
    hexBuf_[hexLen_++] = c;
    if (hexLen_ == 4) {
      decodeUnicodeEscape();
      hexLen_ = 0;
    }
    return;
  }

  appendDecoded(static_cast<uint8_t>(c));
}

void ReadwiseJsonParser::handleNumber(const char c) {
  if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E') {
    return;  // numbers are not captured
  }
  state_ = State::SCANNING;
  expectingValue_ = false;
  handleScanning(c);
}

void ReadwiseJsonParser::handleLiteral(const char c) {
  // Literals are never captured; just consume until the word ends. A literal
  // word only contains lowercase letters, and any non-letter terminates it.
  constexpr uint8_t LITERAL_LENGTHS[] = {4, 5, 4};  // true, false, null
  if (c >= 'a' && c <= 'z') {
    ++literalLen_;
    return;
  }
  state_ = State::SCANNING;
  expectingValue_ = false;
  (void)LITERAL_LENGTHS;  // lengths unused: any non-letter ends the token
  handleScanning(c);
}
