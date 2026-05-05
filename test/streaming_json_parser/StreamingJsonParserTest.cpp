#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lib/JsonParser/StreamingJsonParser.h"

static int testsPassed = 0;
static int testsFailed = 0;

#define ASSERT_EQ(a, b)                                                           \
  do {                                                                            \
    auto _a = (a);                                                                \
    auto _b = (b);                                                                \
    if (_a != _b) {                                                               \
      fprintf(stderr, "  FAIL: %s:%d: %s != expected\n", __FILE__, __LINE__, #a); \
      testsFailed++;                                                              \
      return;                                                                     \
    }                                                                             \
  } while (0)

#define ASSERT_TRUE(cond)                                                \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      testsFailed++;                                                     \
      return;                                                            \
    }                                                                    \
  } while (0)

#define PASS() testsPassed++

// Event types for recording callback sequences
enum class EventType {
  KEY,
  STRING,
  NUMBER,
  BOOL_TRUE,
  BOOL_FALSE,
  NULL_VAL,
  OBJECT_START,
  OBJECT_END,
  ARRAY_START,
  ARRAY_END,
};

struct Event {
  EventType type;
  std::string value;
};

struct TestContext {
  std::vector<Event> events;
};

static void onKey(void* ctx, const char* key, size_t len) {
  static_cast<TestContext*>(ctx)->events.push_back({EventType::KEY, std::string(key, len)});
}
static void onString(void* ctx, const char* value, size_t len) {
  static_cast<TestContext*>(ctx)->events.push_back({EventType::STRING, std::string(value, len)});
}
static void onNumber(void* ctx, const char* value, size_t len) {
  static_cast<TestContext*>(ctx)->events.push_back({EventType::NUMBER, std::string(value, len)});
}
static void onBool(void* ctx, bool value) {
  static_cast<TestContext*>(ctx)->events.push_back({value ? EventType::BOOL_TRUE : EventType::BOOL_FALSE, {}});
}
static void onNull(void* ctx) { static_cast<TestContext*>(ctx)->events.push_back({EventType::NULL_VAL, {}}); }
static void onObjectStart(void* ctx) {
  static_cast<TestContext*>(ctx)->events.push_back({EventType::OBJECT_START, {}});
}
static void onObjectEnd(void* ctx) { static_cast<TestContext*>(ctx)->events.push_back({EventType::OBJECT_END, {}}); }
static void onArrayStart(void* ctx) { static_cast<TestContext*>(ctx)->events.push_back({EventType::ARRAY_START, {}}); }
static void onArrayEnd(void* ctx) { static_cast<TestContext*>(ctx)->events.push_back({EventType::ARRAY_END, {}}); }

static JsonCallbacks makeCallbacks(TestContext* ctx) {
  return {ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd};
}

// Feed entire input at once
static std::vector<Event> parse(const char* json) {
  TestContext ctx;
  StreamingJsonParser parser(makeCallbacks(&ctx));
  parser.feed(json, strlen(json));
  return ctx.events;
}

// Feed input one byte at a time
static std::vector<Event> parseBytewise(const char* json) {
  TestContext ctx;
  StreamingJsonParser parser(makeCallbacks(&ctx));
  size_t len = strlen(json);
  for (size_t i = 0; i < len; ++i) {
    parser.feed(json + i, 1);
  }
  return ctx.events;
}

// ============================================================================
// Tests
// ============================================================================

void testSimpleObject() {
  printf("testSimpleObject...\n");

  auto events = parse(R"({"key": "value", "num": 42})");

  ASSERT_EQ(events.size(), 6u);
  ASSERT_EQ(events[0].type, EventType::OBJECT_START);
  ASSERT_EQ(events[1].type, EventType::KEY);
  ASSERT_EQ(events[1].value, "key");
  ASSERT_EQ(events[2].type, EventType::STRING);
  ASSERT_EQ(events[2].value, "value");
  ASSERT_EQ(events[3].type, EventType::KEY);
  ASSERT_EQ(events[3].value, "num");
  ASSERT_EQ(events[4].type, EventType::NUMBER);
  ASSERT_EQ(events[4].value, "42");
  ASSERT_EQ(events[5].type, EventType::OBJECT_END);

  printf("  passed\n");
  PASS();
}

void testNestedObjects() {
  printf("testNestedObjects...\n");

  auto events = parse(R"({"a": {"b": "c"}})");

  ASSERT_EQ(events.size(), 7u);
  ASSERT_EQ(events[0].type, EventType::OBJECT_START);
  ASSERT_EQ(events[1].type, EventType::KEY);
  ASSERT_EQ(events[1].value, "a");
  ASSERT_EQ(events[2].type, EventType::OBJECT_START);
  ASSERT_EQ(events[3].type, EventType::KEY);
  ASSERT_EQ(events[3].value, "b");
  ASSERT_EQ(events[4].type, EventType::STRING);
  ASSERT_EQ(events[4].value, "c");
  ASSERT_EQ(events[5].type, EventType::OBJECT_END);
  ASSERT_EQ(events[6].type, EventType::OBJECT_END);

  printf("  passed\n");
  PASS();
}

void testArrayOfValues() {
  printf("testArrayOfValues...\n");

  auto events = parse(R"({"items": [1, "two", true, false, null]})");

  ASSERT_EQ(events.size(), 10u);
  ASSERT_EQ(events[0].type, EventType::OBJECT_START);
  ASSERT_EQ(events[1].type, EventType::KEY);
  ASSERT_EQ(events[1].value, "items");
  ASSERT_EQ(events[2].type, EventType::ARRAY_START);
  ASSERT_EQ(events[3].type, EventType::NUMBER);
  ASSERT_EQ(events[3].value, "1");
  ASSERT_EQ(events[4].type, EventType::STRING);
  ASSERT_EQ(events[4].value, "two");
  ASSERT_EQ(events[5].type, EventType::BOOL_TRUE);
  ASSERT_EQ(events[6].type, EventType::BOOL_FALSE);
  ASSERT_EQ(events[7].type, EventType::NULL_VAL);
  ASSERT_EQ(events[8].type, EventType::ARRAY_END);
  ASSERT_EQ(events[9].type, EventType::OBJECT_END);

  printf("  passed\n");
  PASS();
}

void testArrayOfObjects() {
  printf("testArrayOfObjects...\n");

  auto events = parse(R"([{"a": 1}, {"b": 2}])");

  ASSERT_EQ(events.size(), 10u);
  ASSERT_EQ(events[0].type, EventType::ARRAY_START);
  ASSERT_EQ(events[1].type, EventType::OBJECT_START);
  ASSERT_EQ(events[2].type, EventType::KEY);
  ASSERT_EQ(events[2].value, "a");
  ASSERT_EQ(events[3].type, EventType::NUMBER);
  ASSERT_EQ(events[3].value, "1");
  ASSERT_EQ(events[4].type, EventType::OBJECT_END);
  ASSERT_EQ(events[5].type, EventType::OBJECT_START);
  ASSERT_EQ(events[6].type, EventType::KEY);
  ASSERT_EQ(events[6].value, "b");
  ASSERT_EQ(events[7].type, EventType::NUMBER);
  ASSERT_EQ(events[7].value, "2");
  ASSERT_EQ(events[8].type, EventType::OBJECT_END);
  ASSERT_EQ(events[9].type, EventType::ARRAY_END);

  printf("  passed\n");
  PASS();
}

void testStringEscapes() {
  printf("testStringEscapes...\n");

  auto events = parse(R"({"esc": "a\"b\\c\/d\ne\tf"})");

  ASSERT_EQ(events.size(), 4u);
  ASSERT_EQ(events[2].type, EventType::STRING);
  ASSERT_EQ(events[2].value, std::string("a\"b\\c/d\ne\tf"));

  printf("  passed\n");
  PASS();
}

void testUnicodeEscapePassthrough() {
  printf("testUnicodeEscapePassthrough...\n");

  auto events = parse(R"({"u": "\u0041\u0042"})");

  ASSERT_EQ(events[2].type, EventType::STRING);
  // \uXXXX passed through as literal \u followed by the hex digits
  ASSERT_EQ(events[2].value, "\\u0041\\u0042");

  printf("  passed\n");
  PASS();
}

void testNumbers() {
  printf("testNumbers...\n");

  auto events = parse(R"({"int": 42, "neg": -7, "flt": 3.14, "exp": 1e10, "nexp": -2.5E-3})");

  ASSERT_EQ(events[2].type, EventType::NUMBER);
  ASSERT_EQ(events[2].value, "42");
  ASSERT_EQ(events[4].type, EventType::NUMBER);
  ASSERT_EQ(events[4].value, "-7");
  ASSERT_EQ(events[6].type, EventType::NUMBER);
  ASSERT_EQ(events[6].value, "3.14");
  ASSERT_EQ(events[8].type, EventType::NUMBER);
  ASSERT_EQ(events[8].value, "1e10");
  ASSERT_EQ(events[10].type, EventType::NUMBER);
  ASSERT_EQ(events[10].value, "-2.5E-3");

  printf("  passed\n");
  PASS();
}

void testBooleansAndNull() {
  printf("testBooleansAndNull...\n");

  auto events = parse(R"({"t": true, "f": false, "n": null})");

  ASSERT_EQ(events[2].type, EventType::BOOL_TRUE);
  ASSERT_EQ(events[4].type, EventType::BOOL_FALSE);
  ASSERT_EQ(events[6].type, EventType::NULL_VAL);

  printf("  passed\n");
  PASS();
}

void testChunkedFeeding() {
  printf("testChunkedFeeding...\n");

  const char* json = R"({"key": "value", "num": 42, "arr": [1, 2]})";
  auto reference = parse(json);

  // Feed byte-by-byte and verify identical event sequence
  auto bytewise = parseBytewise(json);

  ASSERT_EQ(bytewise.size(), reference.size());
  for (size_t i = 0; i < reference.size(); ++i) {
    ASSERT_EQ(bytewise[i].type, reference[i].type);
    ASSERT_EQ(bytewise[i].value, reference[i].value);
  }

  // Feed in chunks of varying size
  for (size_t chunkSize = 2; chunkSize <= 7; ++chunkSize) {
    TestContext ctx;
    StreamingJsonParser parser(makeCallbacks(&ctx));
    size_t len = strlen(json);
    for (size_t offset = 0; offset < len; offset += chunkSize) {
      size_t remaining = len - offset;
      size_t feedLen = remaining < chunkSize ? remaining : chunkSize;
      parser.feed(json + offset, feedLen);
    }

    ASSERT_EQ(ctx.events.size(), reference.size());
    for (size_t i = 0; i < reference.size(); ++i) {
      ASSERT_EQ(ctx.events[i].type, reference[i].type);
      ASSERT_EQ(ctx.events[i].value, reference[i].value);
    }
  }

  printf("  passed (byte-by-byte + chunk sizes 2-7)\n");
  PASS();
}

void testEveryByteBoundary() {
  printf("testEveryByteBoundary...\n");

  const char* json = R"({"tag_name":"v1.2.3","assets":[{"name":"firmware.bin","size":12345}]})";
  auto reference = parse(json);
  size_t len = strlen(json);

  for (size_t split = 0; split <= len; ++split) {
    TestContext ctx;
    StreamingJsonParser parser(makeCallbacks(&ctx));
    if (split > 0) parser.feed(json, split);
    if (split < len) parser.feed(json + split, len - split);

    ASSERT_EQ(ctx.events.size(), reference.size());
    for (size_t i = 0; i < reference.size(); ++i) {
      if (ctx.events[i].type != reference[i].type || ctx.events[i].value != reference[i].value) {
        fprintf(stderr, "  FAIL at split=%zu, event %zu\n", split, i);
        testsFailed++;
        return;
      }
    }
  }

  printf("  passed (all %zu split points)\n", len + 1);
  PASS();
}

void testLargeTokenTruncation() {
  printf("testLargeTokenTruncation...\n");

  // Build a string value that exceeds TOKEN_BUF_SIZE
  std::string longVal(StreamingJsonParser::TOKEN_BUF_SIZE + 100, 'x');
  std::string json = R"({"short": "ok", "long": ")" + longVal + R"("})";

  auto events = parse(json.c_str());

  // "short" key + "ok" value should still fire
  ASSERT_TRUE(events.size() >= 3);
  ASSERT_EQ(events[1].type, EventType::KEY);
  ASSERT_EQ(events[1].value, "short");
  ASSERT_EQ(events[2].type, EventType::STRING);
  ASSERT_EQ(events[2].value, "ok");

  // The "long" key fires, but the oversized value is silently dropped
  bool foundLongKey = false;
  bool foundLongValue = false;
  for (auto& e : events) {
    if (e.type == EventType::KEY && e.value == "long") foundLongKey = true;
    if (e.type == EventType::STRING && e.value.size() > 500) foundLongValue = true;
  }
  ASSERT_TRUE(foundLongKey);
  ASSERT_TRUE(!foundLongValue);

  printf("  passed\n");
  PASS();
}

void testEmptyObject() {
  printf("testEmptyObject...\n");

  auto events = parse("{}");
  ASSERT_EQ(events.size(), 2u);
  ASSERT_EQ(events[0].type, EventType::OBJECT_START);
  ASSERT_EQ(events[1].type, EventType::OBJECT_END);

  printf("  passed\n");
  PASS();
}

void testEmptyArray() {
  printf("testEmptyArray...\n");

  auto events = parse("[]");
  ASSERT_EQ(events.size(), 2u);
  ASSERT_EQ(events[0].type, EventType::ARRAY_START);
  ASSERT_EQ(events[1].type, EventType::ARRAY_END);

  printf("  passed\n");
  PASS();
}

void testNestedArrays() {
  printf("testNestedArrays...\n");

  auto events = parse("[[1, 2], [3]]");
  ASSERT_EQ(events.size(), 9u);
  ASSERT_EQ(events[0].type, EventType::ARRAY_START);
  ASSERT_EQ(events[1].type, EventType::ARRAY_START);
  ASSERT_EQ(events[2].type, EventType::NUMBER);
  ASSERT_EQ(events[2].value, "1");
  ASSERT_EQ(events[3].type, EventType::NUMBER);
  ASSERT_EQ(events[3].value, "2");
  ASSERT_EQ(events[4].type, EventType::ARRAY_END);
  ASSERT_EQ(events[5].type, EventType::ARRAY_START);
  ASSERT_EQ(events[6].type, EventType::NUMBER);
  ASSERT_EQ(events[6].value, "3");
  ASSERT_EQ(events[7].type, EventType::ARRAY_END);
  ASSERT_EQ(events[8].type, EventType::ARRAY_END);

  printf("  passed\n");
  PASS();
}

void testTopLevelArray() {
  printf("testTopLevelArray...\n");

  auto events = parse(R"(["hello", 42, true, null])");
  ASSERT_EQ(events.size(), 6u);
  ASSERT_EQ(events[0].type, EventType::ARRAY_START);
  ASSERT_EQ(events[1].type, EventType::STRING);
  ASSERT_EQ(events[1].value, "hello");
  ASSERT_EQ(events[2].type, EventType::NUMBER);
  ASSERT_EQ(events[2].value, "42");
  ASSERT_EQ(events[3].type, EventType::BOOL_TRUE);
  ASSERT_EQ(events[4].type, EventType::NULL_VAL);
  ASSERT_EQ(events[5].type, EventType::ARRAY_END);

  printf("  passed\n");
  PASS();
}

void testWhitespaceVariants() {
  printf("testWhitespaceVariants...\n");

  // Minified
  auto minified = parse(R"({"a":1,"b":"x"})");

  // Pretty-printed
  const char* pretty = "{\n  \"a\": 1,\n  \"b\": \"x\"\n}";
  auto prettyEvents = parse(pretty);

  ASSERT_EQ(minified.size(), prettyEvents.size());
  for (size_t i = 0; i < minified.size(); ++i) {
    ASSERT_EQ(minified[i].type, prettyEvents[i].type);
    ASSERT_EQ(minified[i].value, prettyEvents[i].value);
  }

  printf("  passed\n");
  PASS();
}

void testResetBetweenDocuments() {
  printf("testResetBetweenDocuments...\n");

  TestContext ctx;
  StreamingJsonParser parser(makeCallbacks(&ctx));

  const char* json1 = R"({"a": 1})";
  parser.feed(json1, strlen(json1));
  ASSERT_EQ(ctx.events.size(), 4u);

  ctx.events.clear();
  parser.reset();

  const char* json2 = R"({"b": 2})";
  parser.feed(json2, strlen(json2));
  ASSERT_EQ(ctx.events.size(), 4u);
  ASSERT_EQ(ctx.events[1].value, "b");
  ASSERT_EQ(ctx.events[2].value, "2");

  printf("  passed\n");
  PASS();
}

void testNumberAtEndOfInput() {
  printf("testNumberAtEndOfInput...\n");

  // Number terminated by end of input (no trailing whitespace or structural char).
  // The parser must emit the number when feed() ends (after a closing brace).
  auto events = parse(R"({"n": 99})");
  bool found = false;
  for (auto& e : events) {
    if (e.type == EventType::NUMBER && e.value == "99") found = true;
  }
  ASSERT_TRUE(found);

  printf("  passed\n");
  PASS();
}

void testArrayOfStrings() {
  printf("testArrayOfStrings...\n");

  auto events = parse(R"(["a", "b", "c"])");

  ASSERT_EQ(events.size(), 5u);
  ASSERT_EQ(events[0].type, EventType::ARRAY_START);
  ASSERT_EQ(events[1].type, EventType::STRING);
  ASSERT_EQ(events[1].value, "a");
  ASSERT_EQ(events[2].type, EventType::STRING);
  ASSERT_EQ(events[2].value, "b");
  ASSERT_EQ(events[3].type, EventType::STRING);
  ASSERT_EQ(events[3].value, "c");
  ASSERT_EQ(events[4].type, EventType::ARRAY_END);

  printf("  passed\n");
  PASS();
}

void testTruncatedInputNoCrash() {
  printf("testTruncatedInputNoCrash...\n");

  // Simulates a connection drop mid-JSON. Parser must not crash.
  const char* truncated[] = {
      R"({"key": "val)", R"({"key": )",  R"({"key)",     R"([1, 2, )",
      R"({"a": tru)",    R"({"a": fal)", R"({"a": nul)", R"({"a": "hello\)",
  };

  for (auto* json : truncated) {
    TestContext ctx;
    StreamingJsonParser parser(makeCallbacks(&ctx));
    parser.feed(json, strlen(json));
    // Just verify no crash; partial results are acceptable
  }

  printf("  passed (no crashes on %d truncated inputs)\n", 8);
  PASS();
}

void testAllEscapeSequences() {
  printf("testAllEscapeSequences...\n");

  auto events = parse(R"({"e": "\b\f\n\r\t\"\\\/"})");
  ASSERT_EQ(events[2].type, EventType::STRING);
  ASSERT_EQ(events[2].value, std::string("\b\f\n\r\t\"\\/"));

  printf("  passed\n");
  PASS();
}

void testObjectInArray() {
  printf("testObjectInArray...\n");

  // After an object closes inside an array, the next string after comma
  // should be correctly identified as a key (inside the next object) or
  // a string value (if directly in the array).
  auto events = parse(R"([{"k":"v"}, "bare"])");

  ASSERT_EQ(events.size(), 7u);
  ASSERT_EQ(events[0].type, EventType::ARRAY_START);
  ASSERT_EQ(events[1].type, EventType::OBJECT_START);
  ASSERT_EQ(events[2].type, EventType::KEY);
  ASSERT_EQ(events[2].value, "k");
  ASSERT_EQ(events[3].type, EventType::STRING);
  ASSERT_EQ(events[3].value, "v");
  ASSERT_EQ(events[4].type, EventType::OBJECT_END);
  ASSERT_EQ(events[5].type, EventType::STRING);
  ASSERT_EQ(events[5].value, "bare");
  ASSERT_EQ(events[6].type, EventType::ARRAY_END);

  printf("  passed\n");
  PASS();
}

void testDeeplyNested() {
  printf("testDeeplyNested...\n");

  // 20 levels of nesting (well within MAX_NESTING=32)
  std::string json;
  for (int i = 0; i < 20; ++i) json += R"({"d":)";
  json += "0";
  for (int i = 0; i < 20; ++i) json += "}";

  auto events = parse(json.c_str());

  // 20 OBJECT_START + 20 KEY + 1 NUMBER + 20 OBJECT_END = 61
  ASSERT_EQ(events.size(), 61u);
  ASSERT_EQ(events[0].type, EventType::OBJECT_START);
  ASSERT_EQ(events[40].type, EventType::NUMBER);
  ASSERT_EQ(events[40].value, "0");
  ASSERT_EQ(events[60].type, EventType::OBJECT_END);

  printf("  passed\n");
  PASS();
}

void testNestingOverflow() {
  printf("testNestingOverflow...\n");

  // Exceed MAX_NESTING -- parser should set error flag, not crash
  std::string json;
  for (size_t i = 0; i < StreamingJsonParser::MAX_NESTING + 5; ++i) json += "[";

  TestContext ctx;
  StreamingJsonParser parser(makeCallbacks(&ctx));
  parser.feed(json.c_str(), json.size());

  ASSERT_TRUE(parser.hasError());

  printf("  passed\n");
  PASS();
}

void testNumberZero() {
  printf("testNumberZero...\n");

  auto events = parse(R"({"z": 0})");
  ASSERT_EQ(events[2].type, EventType::NUMBER);
  ASSERT_EQ(events[2].value, "0");

  printf("  passed\n");
  PASS();
}

void testMultipleValuesInObject() {
  printf("testMultipleValuesInObject...\n");

  auto events = parse(R"({"a": "x", "b": "y", "c": "z"})");

  ASSERT_EQ(events.size(), 8u);
  ASSERT_EQ(events[1].value, "a");
  ASSERT_EQ(events[2].value, "x");
  ASSERT_EQ(events[3].value, "b");
  ASSERT_EQ(events[4].value, "y");
  ASSERT_EQ(events[5].value, "c");
  ASSERT_EQ(events[6].value, "z");

  printf("  passed\n");
  PASS();
}

void testChunkedSplitInsideString() {
  printf("testChunkedSplitInsideString...\n");

  const char* json = R"({"key": "hello world"})";
  auto reference = parse(json);

  // Split right in the middle of "hello world"
  size_t splitAt = 14;  // inside the string value
  TestContext ctx;
  StreamingJsonParser parser(makeCallbacks(&ctx));
  parser.feed(json, splitAt);
  parser.feed(json + splitAt, strlen(json) - splitAt);

  ASSERT_EQ(ctx.events.size(), reference.size());
  for (size_t i = 0; i < reference.size(); ++i) {
    ASSERT_EQ(ctx.events[i].type, reference[i].type);
    ASSERT_EQ(ctx.events[i].value, reference[i].value);
  }

  printf("  passed\n");
  PASS();
}

void testChunkedSplitInsideEscape() {
  printf("testChunkedSplitInsideEscape...\n");

  const char* json = R"({"k": "a\"b"})";
  auto reference = parse(json);

  // Find the backslash position and split right after it
  const char* bs = strchr(json + 7, '\\');
  ASSERT_TRUE(bs != nullptr);
  size_t splitAt = static_cast<size_t>(bs - json) + 1;  // after the backslash

  TestContext ctx;
  StreamingJsonParser parser(makeCallbacks(&ctx));
  parser.feed(json, splitAt);
  parser.feed(json + splitAt, strlen(json) - splitAt);

  ASSERT_EQ(ctx.events.size(), reference.size());
  for (size_t i = 0; i < reference.size(); ++i) {
    ASSERT_EQ(ctx.events[i].type, reference[i].type);
    ASSERT_EQ(ctx.events[i].value, reference[i].value);
  }

  printf("  passed\n");
  PASS();
}

void testChunkedSplitInsideLiteral() {
  printf("testChunkedSplitInsideLiteral...\n");

  const char* json = R"({"a": true, "b": false, "c": null})";
  auto reference = parse(json);

  // Split inside "true" (at "tr|ue")
  size_t splitAt = 7;
  TestContext ctx;
  StreamingJsonParser parser(makeCallbacks(&ctx));
  parser.feed(json, splitAt);
  parser.feed(json + splitAt, strlen(json) - splitAt);

  ASSERT_EQ(ctx.events.size(), reference.size());
  for (size_t i = 0; i < reference.size(); ++i) {
    ASSERT_EQ(ctx.events[i].type, reference[i].type);
    ASSERT_EQ(ctx.events[i].value, reference[i].value);
  }

  printf("  passed\n");
  PASS();
}

void testNullCallbacksNoCrash() {
  printf("testNullCallbacksNoCrash...\n");

  JsonCallbacks nullCbs = {};
  nullCbs.ctx = nullptr;
  StreamingJsonParser parser(nullCbs);

  const char* json = R"({"key": "value", "num": 42, "b": true, "n": null, "a": [1]})";
  parser.feed(json, strlen(json));
  ASSERT_TRUE(!parser.hasError());

  printf("  passed\n");
  PASS();
}

// ============================================================================

int main() {
  printf("=== StreamingJsonParser Tests ===\n\n");

  testSimpleObject();
  testNestedObjects();
  testArrayOfValues();
  testArrayOfObjects();
  testStringEscapes();
  testUnicodeEscapePassthrough();
  testNumbers();
  testBooleansAndNull();
  testChunkedFeeding();
  testEveryByteBoundary();
  testLargeTokenTruncation();
  testEmptyObject();
  testEmptyArray();
  testNestedArrays();
  testTopLevelArray();
  testWhitespaceVariants();
  testResetBetweenDocuments();
  testNumberAtEndOfInput();
  testArrayOfStrings();
  testTruncatedInputNoCrash();
  testAllEscapeSequences();
  testObjectInArray();
  testDeeplyNested();
  testNestingOverflow();
  testNumberZero();
  testMultipleValuesInObject();
  testChunkedSplitInsideString();
  testChunkedSplitInsideEscape();
  testChunkedSplitInsideLiteral();
  testNullCallbacksNoCrash();

  printf("\n=== Results: %d passed, %d failed ===\n", testsPassed, testsFailed);
  return testsFailed > 0 ? 1 : 0;
}
