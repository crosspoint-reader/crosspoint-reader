#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Incremental JSON scanner tailored to Readwise Reader API responses
 * ({"count":..,"nextPageCursor":".."|null,"results":[{doc},..]}).
 *
 * Captures only the string fields we need (id, title, author, html_content,
 * nextPageCursor) and delivers them as decoded chunks, so arbitrarily long
 * article HTML streams through without buffering. All other content is
 * skipped escape-aware.
 */
class ReadwiseJsonParser {
 public:
  static constexpr size_t KEY_BUF_SIZE = 32;

  struct Callbacks {
    void* ctx = nullptr;
    // Decoded chunk of a captured field; finalChunk=true closes it.
    // Return non-zero to abort the feed (e.g. user cancel / sink error).
    int (*onField)(void* ctx, const char* key, const char* chunk, size_t len, bool finalChunk) = nullptr;
    void (*onDocumentStart)(void* ctx) = nullptr;
    void (*onDocumentEnd)(void* ctx) = nullptr;
  };

  explicit ReadwiseJsonParser(const Callbacks& callbacks) : cb(callbacks) { reset(); }

  void reset();
  void feed(const char* data, size_t len);
  bool hasError() const { return error_; }

 private:
  enum class State : uint8_t { SCANNING, IN_KEY, IN_STRING, IN_SKIP_STRING, IN_NUMBER, IN_LITERAL };
  enum class Container : uint8_t { OBJECT, ARRAY };
  // \uXXXX decoding state
  enum class UnicodeState : uint8_t { NONE, HEX1, SURROGATE_HEX1 };

  static constexpr size_t MAX_DEPTH = 12;
  static constexpr size_t OUT_BUF_SIZE = 256;

  void handleScanning(char c);
  void handleKeyChar(char c);
  void handleStringChar(char c);
  void handleNumber(char c);
  void handleLiteral(char c);

  bool captureCurrentString() const;
  void emitDecoded(const char* bytes, size_t len);
  void flushOutput(bool finalChunk);
  bool appendDecoded(uint8_t byte);
  void decodeUnicodeEscape();
  void pushContainer(Container c);
  void popContainer();

  Callbacks cb;

  State state_ = State::SCANNING;
  Container stack_[MAX_DEPTH];
  uint8_t depth_ = 0;
  bool expectingValue_ = false;

  char keyBuf_[KEY_BUF_SIZE];
  size_t keyLen_ = 0;
  bool keyOverflow_ = false;

  int resultsArrayDepth_ = -1;  // depth of the "results" array itself
  int docObjectDepth_ = -1;     // depth of the current document object

  char outBuf_[OUT_BUF_SIZE];
  size_t outLen_ = 0;

  UnicodeState unicode_ = UnicodeState::NONE;
  char hexBuf_[4];
  uint8_t hexLen_ = 0;
  uint16_t pendingSurrogate_ = 0;
  uint8_t literalLen_ = 0;
  bool escaped_ = false;
  bool error_ = false;
};
