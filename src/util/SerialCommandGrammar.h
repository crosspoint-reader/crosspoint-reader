#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// The argument grammar and reply formatting of the serial test commands (CMD:..., see
// SerialTestApi.h and docs/contributing/serial-test-api.md), kept apart from the dispatcher so it
// can be tested on the host. Nothing here touches the device or the heap.
namespace SerialCommandGrammar {

// Leading and trailing spaces, tabs and a trailing '\r' removed.
std::string_view trim(std::string_view text);

// "NAME" and "rest" of "NAME:rest"; rest is empty when there is no ':'.
void splitHead(std::string_view text, std::string_view& head, std::string_view& rest);

// A decimal integer, optionally signed, with nothing else around it and within int range.
bool parseInt(std::string_view text, int& out);

// How CMD:BTN:<name>[:LONG|:DOUBLE|:<ms>] presses the button.
enum class PressKind : uint8_t { Short, Long, Double, Hold };

// Splits "<NAME>[:LONG|:DOUBLE|:<ms>]". For :<ms>, `holdMs` is 1..maxHoldMs. False for anything
// else, so a typo is an error rather than a short press of a button called "BACK:LONGG".
bool parseButtonArg(std::string_view arg, std::string_view& name, PressKind& kind, int& holdMs, int maxHoldMs);

// One reply line, built in a caller's fixed buffer. What does not fit is dropped and the line is
// marked truncated.
class ReplyLine {
 public:
  ReplyLine(char* buffer, size_t capacity) : buffer(buffer), capacity(capacity) {}

  void append(std::string_view text);
  // " <key>=<value>". A value that is empty or holds a space, '"', '\' or '=' is written in double
  // quotes with '"' and '\' escaped by a backslash, so a host can split the line on spaces outside
  // quotes. Control characters become spaces: the reply must stay one line.
  void appendField(const char* key, std::string_view value);
  void appendField(const char* key, long value);

  std::string_view view() const { return {buffer, length}; }
  bool truncated() const { return cut; }

 private:
  void put(char c);

  char* buffer;
  size_t capacity;
  size_t length = 0;
  bool cut = false;
};

}  // namespace SerialCommandGrammar
