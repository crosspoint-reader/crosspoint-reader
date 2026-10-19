#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// The argument grammar of the serial test commands (CMD:..., see SerialTestApi.h and
// docs/serial-test-api.md), kept apart from the dispatcher so it can be tested on the host. Nothing
// here touches the device: every function reads a view of the command line and fills plain values.
namespace SerialCommandGrammar {

// How CMD:BTN:<name>[:LONG|:DOUBLE] presses the button.
enum class PressKind : uint8_t { Short, Long, Double };

// Splits "<NAME>[:LONG|:DOUBLE]". False for any other suffix, so a typo is an error rather than a
// short press of a button called "BACK:LONGG".
bool parseButtonArg(std::string_view arg, std::string_view& name, PressKind& kind);

// Where CMD:ACT:READER:<path>[@<spine>:<page>|@<percent>%] opens the book. Spine and page are
// 1-based, as CMD:STATE reports them.
struct ReaderTarget {
  enum class Kind : uint8_t { Saved, SpinePage, Percent };
  std::string_view path;
  Kind kind = Kind::Saved;
  int spine = 0;
  int page = 0;
  int percent = 0;
};

// The position is the text after the LAST '@', and only when all of it fits one of the two forms:
// a file name may contain '@' itself, and "/books/a@b.epub" must still open that book.
// False when a position is given but out of range (spine or page below 1, percent above 100).
bool parseReaderTarget(std::string_view arg, ReaderTarget& out);

// Splits "<key>=<value>" at the first '='. False when there is no '=' or the key is empty.
bool splitKeyValue(std::string_view arg, std::string_view& key, std::string_view& value);

// A decimal integer, optionally negative, with nothing else around it.
bool parseInt(std::string_view text, int& out);

// Exactly `count` comma-separated integers ("x,y" for a tap, "x1,y1,x2,y2" for a swipe).
bool parseIntList(std::string_view text, int* out, size_t count);

// CMD:HEAP's ballast: "HOLD=<bytes>" or "HOLD=<bytes>,<blocks>", the bytes split over 1..maxBlocks equal blocks.
bool parseHeapHold(std::string_view arg, int& bytes, int& blocks, int maxBlocks);
// CMD:HEAP's refusals: "REFUSE=<decodes>" or "REFUSE=<decodes>,<reloads>", each 0..255.
bool parseHeapRefuse(std::string_view arg, int& decodes, int& reloads);

// The log levels of lib/Logging (0 = ERR .. 3 = TRC), by their tag or its long name:
// ERR|ERROR, INF|INFO, DBG|DEBUG, TRC|TRACE. Case-insensitive.
bool parseLogLevel(std::string_view text, uint8_t& level);

// A UI language for CMD:SET:language=, out of `count` languages given by their codes (EN, DE, ZH...)
// and native names: its code in any case, its name exactly as the picker shows it, or its index.
bool parseLanguage(std::string_view text, const char* const* codes, const char* const* names, size_t count,
                   size_t& index);

// Appends " <key>=<value>" to a CMD:STATE line. A value that holds a space, a quote, a backslash or
// an '=' is written in double quotes with '"' and '\' escaped by a backslash, so a host can split
// the line on spaces outside quotes; control characters (a newline in a popup's text) become
// spaces, since the line must stay one line.
void appendField(std::string& out, const char* key, std::string_view value);
void appendField(std::string& out, const char* key, long value);
// Just the value half of appendField(), quoted by the same rule.
void appendValue(std::string& out, std::string_view value);

}  // namespace SerialCommandGrammar
