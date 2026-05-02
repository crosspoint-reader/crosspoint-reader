#pragma once

#include <cstddef>
#include <cstdint>

struct JsonCallbacks {
  void* ctx;
  void (*onKey)(void* ctx, const char* key, size_t len);
  void (*onString)(void* ctx, const char* value, size_t len);
  void (*onNumber)(void* ctx, const char* value, size_t len);
  void (*onBool)(void* ctx, bool value);
  void (*onNull)(void* ctx);
  void (*onObjectStart)(void* ctx);
  void (*onObjectEnd)(void* ctx);
  void (*onArrayStart)(void* ctx);
  void (*onArrayEnd)(void* ctx);
};

class StreamingJsonParser {
 public:
  static constexpr size_t TOKEN_BUF_SIZE = 512;
  static constexpr size_t MAX_NESTING = 32;

  explicit StreamingJsonParser(const JsonCallbacks& callbacks);

  void reset();
  void feed(const char* data, size_t len);

  bool hasError() const { return error; }

 private:
  enum class State : uint8_t {
    SCANNING,
    IN_STRING_KEY,
    IN_STRING_VALUE,
    IN_NUMBER,
    IN_LITERAL,
    SKIP_STRING,
  };

  enum class Container : uint8_t {
    NONE,
    OBJECT,
    ARRAY,
  };

  void handleScanning(char c);
  void handleStringChar(char c);
  void handleNumber(char c);
  void handleLiteral(char c);
  void handleSkipString(char c);

  void appendToken(char c);
  void emitToken();

  bool inArray() const { return nestingDepth > 0 && nestingStack[nestingDepth - 1] == Container::ARRAY; }

  JsonCallbacks cb;
  char tokenBuf[TOKEN_BUF_SIZE];
  size_t tokenLen;
  State state;
  bool expectingValue;
  bool escaped;
  bool tokenOverflow;
  bool error;

  Container nestingStack[MAX_NESTING];
  uint8_t nestingDepth;

  char literalExpected[6];
  uint8_t literalLen;
  uint8_t literalPos;
};
