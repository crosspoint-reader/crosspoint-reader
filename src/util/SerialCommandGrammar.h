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
  // appendField(), with the value cut short if needed so that `reserve` bytes stay free. The cut
  // falls before a UTF-8 character and leaves the quotes whole. False when the value was cut. At
  // least ` key=""` is written; when even that does not fit, the line is truncated().
  bool appendFieldClipped(const char* key, std::string_view value, size_t reserve);

  std::string_view view() const { return {buffer, length}; }
  bool truncated() const { return cut; }

 private:
  void put(char c);

  char* buffer;
  size_t capacity;
  size_t length = 0;
  bool cut = false;
};

// A free-text field put last on a reply line (STATE's stack and title).
struct TailField {
  const char* key;
  std::string_view value;
  bool cut = false;  // set when the value was cut, here or before (a list cut by its own buffer)
};

// Appends `fields` as the line's last fields, each clipped so that the fields after it keep room
// for an empty value and for a last field "clipped=<key>[,<key>...]" naming the cut ones. False
// when the line overflowed anyway: it must not be sent.
bool appendClippedTail(ReplyLine& line, TailField* fields, size_t count);

}  // namespace SerialCommandGrammar
