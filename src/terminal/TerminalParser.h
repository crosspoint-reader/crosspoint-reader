#pragma once

#include <cstdint>

#include "TerminalScreen.h"

class TerminalParser {
 public:
  explicit TerminalParser(TerminalScreen& screen) : screen(screen) {}

  void reset();
  void feed(uint8_t byte);

 private:
  enum class State : uint8_t {
    Ground,
    Escape,
    EscapeCharset,
    Csi,
    ControlString,
    ControlStringEscape,
  };
  static constexpr uint8_t MAX_PARAMS = 8;

  TerminalScreen& screen;
  State state = State::Ground;
  uint16_t params[MAX_PARAMS]{};
  uint8_t paramCount = 0;
  bool paramPresent = false;
  uint8_t privateMarker = 0;
  uint8_t intermediate = 0;
  bool controlStringAcceptsBel = false;
  uint8_t scrollTop = 0;
  uint8_t scrollBottom = TerminalScreen::ROWS - 1;
  uint8_t savedCursorRow = 0;
  uint8_t savedCursorColumn = 0;
  uint8_t savedAttributes = TerminalScreen::ATTR_NONE;
  bool savedCursorValid = false;
  uint32_t utf8Codepoint = 0;
  uint32_t utf8Minimum = 0;
  uint8_t utf8Remaining = 0;

  void beginCsi();
  void resetUtf8();
  void feedGround(uint8_t byte);
  void feedEscape(uint8_t byte);
  void feedControlString(uint8_t byte);
  void finishParam();
  uint16_t parameter(uint8_t index, uint16_t defaultValue, bool zeroMeansDefault = false) const;
  void dispatch(uint8_t finalByte);
  void dispatchSgr();
};
