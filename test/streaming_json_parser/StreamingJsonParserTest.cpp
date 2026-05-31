#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "lib/JsonParser/StreamingJsonParser.h"

namespace {

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

void onKey(void* ctx, const char* key, size_t len) {
  static_cast<TestContext*>(ctx)->events.push_back({EventType::KEY, std::string(key, len)});
}
void onString(void* ctx, const char* value, size_t len) {
  static_cast<TestContext*>(ctx)->events.push_back({EventType::STRING, std::string(value, len)});
}
void onNumber(void* ctx, const char* value, size_t len) {
  static_cast<TestContext*>(ctx)->events.push_back({EventType::NUMBER, std::string(value, len)});
}
void onBool(void* ctx, bool value) {
  static_cast<TestContext*>(ctx)->events.push_back({value ? EventType::BOOL_TRUE : EventType::BOOL_FALSE, {}});
}
void onNull(void* ctx) { static_cast<TestContext*>(ctx)->events.push_back({EventType::NULL_VAL, {}}); }
void onObjectStart(void* ctx) { static_cast<TestContext*>(ctx)->events.push_back({EventType::OBJECT_START, {}}); }
void onObjectEnd(void* ctx) { static_cast<TestContext*>(ctx)->events.push_back({EventType::OBJECT_END, {}}); }
void onArrayStart(void* ctx) { static_cast<TestContext*>(ctx)->events.push_back({EventType::ARRAY_START, {}}); }
void onArrayEnd(void* ctx) { static_cast<TestContext*>(ctx)->events.push_back({EventType::ARRAY_END, {}}); }

JsonCallbacks makeCallbacks(TestContext* ctx) {
  return {ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd};
}

std::vector<Event> parse(const char* json) {
  TestContext ctx;
  StreamingJsonParser parser(makeCallbacks(&ctx));
  parser.feed(json, strlen(json));
  return ctx.events;
}

std::vector<Event> parseBytewise(const char* json) {
  TestContext ctx;
  StreamingJsonParser parser(makeCallbacks(&ctx));
  size_t len = strlen(json);
  for (size_t i = 0; i < len; ++i) {
    parser.feed(json + i, 1);
  }
  return ctx.events;
}

}  // namespace

TEST(StreamingJsonParser, SimpleObject) {
  auto events = parse(R"({"key": "value", "num": 42})");

  ASSERT_EQ(events.size(), 6u);
  EXPECT_EQ(events[0].type, EventType::OBJECT_START);
  EXPECT_EQ(events[1].type, EventType::KEY);
  EXPECT_EQ(events[1].value, "key");
  EXPECT_EQ(events[2].type, EventType::STRING);
  EXPECT_EQ(events[2].value, "value");
  EXPECT_EQ(events[3].type, EventType::KEY);
  EXPECT_EQ(events[3].value, "num");
  EXPECT_EQ(events[4].type, EventType::NUMBER);
  EXPECT_EQ(events[4].value, "42");
  EXPECT_EQ(events[5].type, EventType::OBJECT_END);
}

TEST(StreamingJsonParser, NestedObjects) {
  auto events = parse(R"({"a": {"b": "c"}})");

  ASSERT_EQ(events.size(), 7u);
  EXPECT_EQ(events[0].type, EventType::OBJECT_START);
  EXPECT_EQ(events[1].type, EventType::KEY);
  EXPECT_EQ(events[1].value, "a");
  EXPECT_EQ(events[2].type, EventType::OBJECT_START);
  EXPECT_EQ(events[3].type, EventType::KEY);
  EXPECT_EQ(events[3].value, "b");
  EXPECT_EQ(events[4].type, EventType::STRING);
  EXPECT_EQ(events[4].value, "c");
  EXPECT_EQ(events[5].type, EventType::OBJECT_END);
  EXPECT_EQ(events[6].type, EventType::OBJECT_END);
}

TEST(StreamingJsonParser, ArrayOfValues) {
  auto events = parse(R"({"items": [1, "two", true, false, null]})");

  ASSERT_EQ(events.size(), 10u);
  EXPECT_EQ(events[0].type, EventType::OBJECT_START);
  EXPECT_EQ(events[1].type, EventType::KEY);
  EXPECT_EQ(events[1].value, "items");
  EXPECT_EQ(events[2].type, EventType::ARRAY_START);
  EXPECT_EQ(events[3].type, EventType::NUMBER);
  EXPECT_EQ(events[3].value, "1");
  EXPECT_EQ(events[4].type, EventType::STRING);
  EXPECT_EQ(events[4].value, "two");
  EXPECT_EQ(events[5].type, EventType::BOOL_TRUE);
  EXPECT_EQ(events[6].type, EventType::BOOL_FALSE);
  EXPECT_EQ(events[7].type, EventType::NULL_VAL);
  EXPECT_EQ(events[8].type, EventType::ARRAY_END);
  EXPECT_EQ(events[9].type, EventType::OBJECT_END);
}

TEST(StreamingJsonParser, ArrayOfObjects) {
  auto events = parse(R"([{"a": 1}, {"b": 2}])");

  ASSERT_EQ(events.size(), 10u);
  EXPECT_EQ(events[0].type, EventType::ARRAY_START);
  EXPECT_EQ(events[1].type, EventType::OBJECT_START);
  EXPECT_EQ(events[2].type, EventType::KEY);
  EXPECT_EQ(events[2].value, "a");
  EXPECT_EQ(events[3].type, EventType::NUMBER);
  EXPECT_EQ(events[3].value, "1");
  EXPECT_EQ(events[4].type, EventType::OBJECT_END);
  EXPECT_EQ(events[5].type, EventType::OBJECT_START);
  EXPECT_EQ(events[6].type, EventType::KEY);
  EXPECT_EQ(events[6].value, "b");
  EXPECT_EQ(events[7].type, EventType::NUMBER);
  EXPECT_EQ(events[7].value, "2");
  EXPECT_EQ(events[8].type, EventType::OBJECT_END);
  EXPECT_EQ(events[9].type, EventType::ARRAY_END);
}

TEST(StreamingJsonParser, StringEscapes) {
  auto events = parse(R"({"esc": "a\"b\\c\/d\ne\tf"})");

  ASSERT_EQ(events.size(), 4u);
  EXPECT_EQ(events[2].type, EventType::STRING);
  EXPECT_EQ(events[2].value, std::string("a\"b\\c/d\ne\tf"));
}

TEST(StreamingJsonParser, UnicodeEscapePassthrough) {
  auto events = parse(R"({"u": "\u0041\u0042"})");

  ASSERT_EQ(events.size(), 4u);
  EXPECT_EQ(events[2].type, EventType::STRING);
  // \uXXXX passed through as literal \u followed by the hex digits
  EXPECT_EQ(events[2].value, "\\u0041\\u0042");
}

TEST(StreamingJsonParser, Numbers) {
  auto events = parse(R"({"int": 42, "neg": -7, "flt": 3.14, "exp": 1e10, "nexp": -2.5E-3})");

  ASSERT_EQ(events.size(), 12u);
  EXPECT_EQ(events[2].type, EventType::NUMBER);
  EXPECT_EQ(events[2].value, "42");
  EXPECT_EQ(events[4].type, EventType::NUMBER);
  EXPECT_EQ(events[4].value, "-7");
  EXPECT_EQ(events[6].type, EventType::NUMBER);
  EXPECT_EQ(events[6].value, "3.14");
  EXPECT_EQ(events[8].type, EventType::NUMBER);
  EXPECT_EQ(events[8].value, "1e10");
  EXPECT_EQ(events[10].type, EventType::NUMBER);
  EXPECT_EQ(events[10].value, "-2.5E-3");
}

TEST(StreamingJsonParser, BooleansAndNull) {
  auto events = parse(R"({"t": true, "f": false, "n": null})");

  ASSERT_EQ(events.size(), 8u);
  EXPECT_EQ(events[2].type, EventType::BOOL_TRUE);
  EXPECT_EQ(events[4].type, EventType::BOOL_FALSE);
  EXPECT_EQ(events[6].type, EventType::NULL_VAL);
}

TEST(StreamingJsonParser, ChunkedFeeding) {
  const char* json = R"({"key": "value", "num": 42, "arr": [1, 2]})";
  auto reference = parse(json);

  auto bytewise = parseBytewise(json);
  ASSERT_EQ(bytewise.size(), reference.size());
  for (size_t i = 0; i < reference.size(); ++i) {
    EXPECT_EQ(bytewise[i].type, reference[i].type);
    EXPECT_EQ(bytewise[i].value, reference[i].value);
  }

  for (size_t chunkSize = 2; chunkSize <= 7; ++chunkSize) {
    TestContext ctx;
    StreamingJsonParser parser(makeCallbacks(&ctx));
    size_t len = strlen(json);
    for (size_t offset = 0; offset < len; offset += chunkSize) {
      size_t remaining = len - offset;
      size_t feedLen = remaining < chunkSize ? remaining : chunkSize;
      parser.feed(json + offset, feedLen);
    }

    ASSERT_EQ(ctx.events.size(), reference.size()) << "chunkSize=" << chunkSize;
    for (size_t i = 0; i < reference.size(); ++i) {
      EXPECT_EQ(ctx.events[i].type, reference[i].type) << "chunkSize=" << chunkSize << " event=" << i;
      EXPECT_EQ(ctx.events[i].value, reference[i].value) << "chunkSize=" << chunkSize << " event=" << i;
    }
  }
}

TEST(StreamingJsonParser, EveryByteBoundary) {
  const char* json = R"({"tag_name":"v1.2.3","assets":[{"name":"firmware.bin","size":12345}]})";
  auto reference = parse(json);
  size_t len = strlen(json);

  for (size_t split = 0; split <= len; ++split) {
    TestContext ctx;
    StreamingJsonParser parser(makeCallbacks(&ctx));
    if (split > 0) parser.feed(json, split);
    if (split < len) parser.feed(json + split, len - split);

    ASSERT_EQ(ctx.events.size(), reference.size()) << "split=" << split;
    for (size_t i = 0; i < reference.size(); ++i) {
      EXPECT_EQ(ctx.events[i].type, reference[i].type) << "split=" << split << " event=" << i;
      EXPECT_EQ(ctx.events[i].value, reference[i].value) << "split=" << split << " event=" << i;
    }
  }
}

TEST(StreamingJsonParser, LargeTokenTruncation) {
  // Build a string value that exceeds TOKEN_BUF_SIZE
  std::string longVal(StreamingJsonParser::TOKEN_BUF_SIZE + 100, 'x');
  std::string json = R"({"short": "ok", "long": ")" + longVal + R"("})";

  auto events = parse(json.c_str());

  ASSERT_GE(events.size(), 3u);
  EXPECT_EQ(events[1].type, EventType::KEY);
  EXPECT_EQ(events[1].value, "short");
  EXPECT_EQ(events[2].type, EventType::STRING);
  EXPECT_EQ(events[2].value, "ok");

  bool foundLongKey = false;
  bool foundLongValue = false;
  for (auto& e : events) {
    if (e.type == EventType::KEY && e.value == "long") foundLongKey = true;
    if (e.type == EventType::STRING && e.value.size() > 500) foundLongValue = true;
  }
  EXPECT_TRUE(foundLongKey);
  EXPECT_FALSE(foundLongValue);
}

TEST(StreamingJsonParser, EmptyObject) {
  auto events = parse("{}");
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].type, EventType::OBJECT_START);
  EXPECT_EQ(events[1].type, EventType::OBJECT_END);
}

TEST(StreamingJsonParser, EmptyArray) {
  auto events = parse("[]");
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].type, EventType::ARRAY_START);
  EXPECT_EQ(events[1].type, EventType::ARRAY_END);
}

TEST(StreamingJsonParser, NestedArrays) {
  auto events = parse("[[1, 2], [3]]");
  ASSERT_EQ(events.size(), 9u);
  EXPECT_EQ(events[0].type, EventType::ARRAY_START);
  EXPECT_EQ(events[1].type, EventType::ARRAY_START);
  EXPECT_EQ(events[2].type, EventType::NUMBER);
  EXPECT_EQ(events[2].value, "1");
  EXPECT_EQ(events[3].type, EventType::NUMBER);
  EXPECT_EQ(events[3].value, "2");
  EXPECT_EQ(events[4].type, EventType::ARRAY_END);
  EXPECT_EQ(events[5].type, EventType::ARRAY_START);
  EXPECT_EQ(events[6].type, EventType::NUMBER);
  EXPECT_EQ(events[6].value, "3");
  EXPECT_EQ(events[7].type, EventType::ARRAY_END);
  EXPECT_EQ(events[8].type, EventType::ARRAY_END);
}

TEST(StreamingJsonParser, TopLevelArray) {
  auto events = parse(R"(["hello", 42, true, null])");
  ASSERT_EQ(events.size(), 6u);
  EXPECT_EQ(events[0].type, EventType::ARRAY_START);
  EXPECT_EQ(events[1].type, EventType::STRING);
  EXPECT_EQ(events[1].value, "hello");
  EXPECT_EQ(events[2].type, EventType::NUMBER);
  EXPECT_EQ(events[2].value, "42");
  EXPECT_EQ(events[3].type, EventType::BOOL_TRUE);
  EXPECT_EQ(events[4].type, EventType::NULL_VAL);
  EXPECT_EQ(events[5].type, EventType::ARRAY_END);
}

TEST(StreamingJsonParser, WhitespaceVariants) {
  auto minified = parse(R"({"a":1,"b":"x"})");
  const char* pretty = "{\n  \"a\": 1,\n  \"b\": \"x\"\n}";
  auto prettyEvents = parse(pretty);

  ASSERT_EQ(minified.size(), prettyEvents.size());
  for (size_t i = 0; i < minified.size(); ++i) {
    EXPECT_EQ(minified[i].type, prettyEvents[i].type);
    EXPECT_EQ(minified[i].value, prettyEvents[i].value);
  }
}

TEST(StreamingJsonParser, ResetBetweenDocuments) {
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
  EXPECT_EQ(ctx.events[1].value, "b");
  EXPECT_EQ(ctx.events[2].value, "2");
}

TEST(StreamingJsonParser, NumberAtEndOfInput) {
  auto events = parse(R"({"n": 99})");
  bool found = false;
  for (auto& e : events) {
    if (e.type == EventType::NUMBER && e.value == "99") found = true;
  }
  EXPECT_TRUE(found);
}

TEST(StreamingJsonParser, ArrayOfStrings) {
  auto events = parse(R"(["a", "b", "c"])");

  ASSERT_EQ(events.size(), 5u);
  EXPECT_EQ(events[0].type, EventType::ARRAY_START);
  EXPECT_EQ(events[1].type, EventType::STRING);
  EXPECT_EQ(events[1].value, "a");
  EXPECT_EQ(events[2].type, EventType::STRING);
  EXPECT_EQ(events[2].value, "b");
  EXPECT_EQ(events[3].type, EventType::STRING);
  EXPECT_EQ(events[3].value, "c");
  EXPECT_EQ(events[4].type, EventType::ARRAY_END);
}

TEST(StreamingJsonParser, TruncatedInputNoCrash) {
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
  SUCCEED();
}

TEST(StreamingJsonParser, AllEscapeSequences) {
  auto events = parse(R"({"e": "\b\f\n\r\t\"\\\/"})");
  ASSERT_EQ(events.size(), 4u);
  EXPECT_EQ(events[2].type, EventType::STRING);
  EXPECT_EQ(events[2].value, std::string("\b\f\n\r\t\"\\/"));
}

TEST(StreamingJsonParser, ObjectInArray) {
  // After an object closes inside an array, the next string after comma
  // should be correctly identified as a key (inside the next object) or
  // a string value (if directly in the array).
  auto events = parse(R"([{"k":"v"}, "bare"])");

  ASSERT_EQ(events.size(), 7u);
  EXPECT_EQ(events[0].type, EventType::ARRAY_START);
  EXPECT_EQ(events[1].type, EventType::OBJECT_START);
  EXPECT_EQ(events[2].type, EventType::KEY);
  EXPECT_EQ(events[2].value, "k");
  EXPECT_EQ(events[3].type, EventType::STRING);
  EXPECT_EQ(events[3].value, "v");
  EXPECT_EQ(events[4].type, EventType::OBJECT_END);
  EXPECT_EQ(events[5].type, EventType::STRING);
  EXPECT_EQ(events[5].value, "bare");
  EXPECT_EQ(events[6].type, EventType::ARRAY_END);
}

TEST(StreamingJsonParser, DeeplyNested) {
  // 20 levels of nesting (well within MAX_NESTING=32)
  std::string json;
  for (int i = 0; i < 20; ++i) json += R"({"d":)";
  json += "0";
  for (int i = 0; i < 20; ++i) json += "}";

  auto events = parse(json.c_str());

  // 20 OBJECT_START + 20 KEY + 1 NUMBER + 20 OBJECT_END = 61
  ASSERT_EQ(events.size(), 61u);
  EXPECT_EQ(events[0].type, EventType::OBJECT_START);
  EXPECT_EQ(events[40].type, EventType::NUMBER);
  EXPECT_EQ(events[40].value, "0");
  EXPECT_EQ(events[60].type, EventType::OBJECT_END);
}

TEST(StreamingJsonParser, NestingOverflow) {
  // Exceed MAX_NESTING -- parser should set error flag, not crash
  std::string json;
  for (size_t i = 0; i < StreamingJsonParser::MAX_NESTING + 5; ++i) json += "[";

  TestContext ctx;
  StreamingJsonParser parser(makeCallbacks(&ctx));
  parser.feed(json.c_str(), json.size());

  EXPECT_TRUE(parser.hasError());
}

TEST(StreamingJsonParser, NumberZero) {
  auto events = parse(R"({"z": 0})");
  ASSERT_EQ(events.size(), 4u);
  EXPECT_EQ(events[2].type, EventType::NUMBER);
  EXPECT_EQ(events[2].value, "0");
}

TEST(StreamingJsonParser, MultipleValuesInObject) {
  auto events = parse(R"({"a": "x", "b": "y", "c": "z"})");

  ASSERT_EQ(events.size(), 8u);
  EXPECT_EQ(events[1].value, "a");
  EXPECT_EQ(events[2].value, "x");
  EXPECT_EQ(events[3].value, "b");
  EXPECT_EQ(events[4].value, "y");
  EXPECT_EQ(events[5].value, "c");
  EXPECT_EQ(events[6].value, "z");
}

TEST(StreamingJsonParser, ChunkedSplitInsideString) {
  const char* json = R"({"key": "hello world"})";
  auto reference = parse(json);

  size_t splitAt = 14;  // inside the string value
  TestContext ctx;
  StreamingJsonParser parser(makeCallbacks(&ctx));
  parser.feed(json, splitAt);
  parser.feed(json + splitAt, strlen(json) - splitAt);

  ASSERT_EQ(ctx.events.size(), reference.size());
  for (size_t i = 0; i < reference.size(); ++i) {
    EXPECT_EQ(ctx.events[i].type, reference[i].type);
    EXPECT_EQ(ctx.events[i].value, reference[i].value);
  }
}

TEST(StreamingJsonParser, ChunkedSplitInsideEscape) {
  const char* json = R"({"k": "a\"b"})";
  auto reference = parse(json);

  const char* bs = strchr(json + 7, '\\');
  ASSERT_NE(bs, nullptr);
  size_t splitAt = static_cast<size_t>(bs - json) + 1;  // after the backslash

  TestContext ctx;
  StreamingJsonParser parser(makeCallbacks(&ctx));
  parser.feed(json, splitAt);
  parser.feed(json + splitAt, strlen(json) - splitAt);

  ASSERT_EQ(ctx.events.size(), reference.size());
  for (size_t i = 0; i < reference.size(); ++i) {
    EXPECT_EQ(ctx.events[i].type, reference[i].type);
    EXPECT_EQ(ctx.events[i].value, reference[i].value);
  }
}

TEST(StreamingJsonParser, ChunkedSplitInsideLiteral) {
  const char* json = R"({"a": true, "b": false, "c": null})";
  auto reference = parse(json);

  size_t splitAt = 7;  // inside "true"
  TestContext ctx;
  StreamingJsonParser parser(makeCallbacks(&ctx));
  parser.feed(json, splitAt);
  parser.feed(json + splitAt, strlen(json) - splitAt);

  ASSERT_EQ(ctx.events.size(), reference.size());
  for (size_t i = 0; i < reference.size(); ++i) {
    EXPECT_EQ(ctx.events[i].type, reference[i].type);
    EXPECT_EQ(ctx.events[i].value, reference[i].value);
  }
}

TEST(StreamingJsonParser, NullCallbacksNoCrash) {
  JsonCallbacks nullCbs = {};
  nullCbs.ctx = nullptr;
  StreamingJsonParser parser(nullCbs);

  const char* json = R"({"key": "value", "num": 42, "b": true, "n": null, "a": [1]})";
  parser.feed(json, strlen(json));
  EXPECT_FALSE(parser.hasError());
}
