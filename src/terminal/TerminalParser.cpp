#include "TerminalParser.h"

#include <algorithm>
#include <iterator>

void TerminalParser::reset() {
  state = State::Ground;
  beginCsi();
  resetUtf8();
  controlStringAcceptsBel = false;
  scrollTop = 0;
  scrollBottom = TerminalScreen::ROWS - 1;
  savedCursorRow = 0;
  savedCursorColumn = 0;
  savedAttributes = TerminalScreen::ATTR_NONE;
  savedCursorValid = false;
}

void TerminalParser::beginCsi() {
  std::fill(std::begin(params), std::end(params), 0);
  paramCount = 0;
  paramPresent = false;
  privateMarker = 0;
  intermediate = 0;
}

void TerminalParser::resetUtf8() {
  utf8Codepoint = 0;
  utf8Minimum = 0;
  utf8Remaining = 0;
}

void TerminalParser::feedGround(const uint8_t byte) {
  if (utf8Remaining > 0) {
    if ((byte & 0xc0) != 0x80) {
      resetUtf8();
      screen.putCodepoint(TerminalScreen::REPLACEMENT_CODEPOINT, scrollTop, scrollBottom);
      feed(byte);
      return;
    }
    utf8Codepoint = (utf8Codepoint << 6) | (byte & 0x3f);
    --utf8Remaining;
    if (utf8Remaining == 0) {
      const uint32_t codepoint = utf8Codepoint;
      const bool valid =
          codepoint >= utf8Minimum && codepoint <= 0x10ffff && !(codepoint >= 0xd800 && codepoint <= 0xdfff);
      resetUtf8();
      screen.putCodepoint(valid ? codepoint : TerminalScreen::REPLACEMENT_CODEPOINT, scrollTop, scrollBottom);
    }
    return;
  }

  if (byte < 0x80) {
    switch (byte) {
      case 0x1b:
        state = State::Escape;
        return;
      case '\n':
        screen.lineFeed(scrollTop, scrollBottom);
        return;
      case '\r':
        screen.carriageReturn();
        return;
      case '\b':
        screen.backspace();
        return;
      case '\t':
        screen.tab();
        return;
      case 0x07:
        return;
      default:
        if (byte >= 0x20) screen.putCodepoint(byte, scrollTop, scrollBottom);
        return;
    }
  }

  if (byte >= 0xc2 && byte <= 0xdf) {
    utf8Codepoint = byte & 0x1f;
    utf8Minimum = 0x80;
    utf8Remaining = 1;
  } else if (byte >= 0xe0 && byte <= 0xef) {
    utf8Codepoint = byte & 0x0f;
    utf8Minimum = 0x800;
    utf8Remaining = 2;
  } else if (byte >= 0xf0 && byte <= 0xf4) {
    utf8Codepoint = byte & 0x07;
    utf8Minimum = 0x10000;
    utf8Remaining = 3;
  } else {
    screen.putCodepoint(TerminalScreen::REPLACEMENT_CODEPOINT, scrollTop, scrollBottom);
  }
}

void TerminalParser::feedEscape(const uint8_t byte) {
  state = State::Ground;
  switch (byte) {
    case '[':
      beginCsi();
      state = State::Csi;
      break;
    case ']':
      controlStringAcceptsBel = true;
      state = State::ControlString;
      break;
    case 'P':
    case 'X':
    case '^':
    case '_':
      controlStringAcceptsBel = false;
      state = State::ControlString;
      break;
    case '(':
    case ')':
    case '*':
    case '+':
      state = State::EscapeCharset;
      break;
    case '7':
      savedCursorRow = screen.getCursorRow();
      savedCursorColumn = screen.getCursorColumn();
      savedAttributes = screen.getAttributes();
      savedCursorValid = true;
      break;
    case '8':
      if (savedCursorValid) {
        screen.setAttributes(savedAttributes);
        screen.setCursor(savedCursorRow + 1, savedCursorColumn + 1);
      }
      break;
    case 'D':
      screen.lineFeed(scrollTop, scrollBottom);
      break;
    case 'E':
      screen.lineFeed(scrollTop, scrollBottom);
      screen.carriageReturn();
      break;
    case 'M':
      screen.reverseIndex(scrollTop, scrollBottom);
      break;
    case 'c':
      screen.reset();
      reset();
      break;
    case 0x1b:
      state = State::Escape;
      break;
    default:
      break;
  }
}

void TerminalParser::feedControlString(const uint8_t byte) {
  if (state == State::ControlStringEscape) {
    if (byte == '\\') {
      state = State::Ground;
    } else if (byte != 0x1b) {
      state = State::ControlString;
    }
    return;
  }

  if ((controlStringAcceptsBel && byte == 0x07) || byte == 0x18 || byte == 0x1a) {
    state = State::Ground;
  } else if (byte == 0x1b) {
    state = State::ControlStringEscape;
  }
}

void TerminalParser::finishParam() {
  if (paramCount < MAX_PARAMS) {
    ++paramCount;
  }
  paramPresent = false;
}

uint16_t TerminalParser::parameter(const uint8_t index, const uint16_t defaultValue,
                                   const bool zeroMeansDefault) const {
  if (index >= paramCount) {
    return defaultValue;
  }
  const uint16_t value = params[index];
  return zeroMeansDefault && value == 0 ? defaultValue : value;
}

void TerminalParser::feed(const uint8_t byte) {
  if (state == State::Ground) return feedGround(byte);
  if (state == State::Escape) return feedEscape(byte);
  if (state == State::EscapeCharset) {
    state = byte == 0x1b ? State::Escape : State::Ground;
    return;
  }
  if (state == State::ControlString || state == State::ControlStringEscape) return feedControlString(byte);

  if (byte == 0x1b) {
    state = State::Escape;
    return;
  }
  if (byte == 0x18 || byte == 0x1a) {
    state = State::Ground;
    return;
  }

  if (byte >= '0' && byte <= '9' && intermediate == 0) {
    paramPresent = true;
    if (paramCount < MAX_PARAMS) {
      params[paramCount] = std::min<uint16_t>(999, params[paramCount] * 10 + (byte - '0'));
    }
    return;
  }
  if ((byte == ';' || byte == ':') && intermediate == 0) {
    finishParam();
    return;
  }
  if (byte >= 0x3c && byte <= 0x3f && paramCount == 0 && !paramPresent && privateMarker == 0 && intermediate == 0) {
    privateMarker = byte;
    return;
  }
  if (byte >= 0x20 && byte <= 0x2f) {
    intermediate = intermediate == 0 ? byte : 0xff;
    return;
  }
  if (byte >= 0x40 && byte <= 0x7e) {
    if (paramPresent || paramCount > 0) {
      finishParam();
    }
    dispatch(byte);
    state = State::Ground;
    return;
  }

  // Consume unsupported parameter bytes until the final byte so their tail
  // cannot leak into the terminal as printable text.
  if (byte < 0x30 || byte > 0x3f) state = State::Ground;
}

void TerminalParser::dispatch(const uint8_t finalByte) {
  if (privateMarker != 0) {
    if (privateMarker == '?' && parameter(0, 0) == 25 && (finalByte == 'h' || finalByte == 'l')) {
      screen.setCursorVisible(finalByte == 'h');
    }
    return;
  }

  // Cursor-style selection (CSI Ps SP q) and other unsupported intermediate
  // forms are intentionally consumed as complete sequences.
  if (intermediate != 0) return;

  switch (finalByte) {
    case 'A':
      screen.moveCursor(-static_cast<int>(parameter(0, 1, true)), 0);
      break;
    case 'B':
      screen.moveCursor(static_cast<int>(parameter(0, 1, true)), 0);
      break;
    case 'C':
      screen.moveCursor(0, static_cast<int>(parameter(0, 1, true)));
      break;
    case 'D':
      screen.moveCursor(0, -static_cast<int>(parameter(0, 1, true)));
      break;
    case 'H':
    case 'f':
      screen.setCursor(parameter(0, 1, true), parameter(1, 1, true));
      break;
    case 'J':
      screen.clearDisplay(parameter(0, 0));
      break;
    case 'K':
      screen.clearLine(parameter(0, 0));
      break;
    case 'S':
      screen.scrollUp(scrollTop, scrollBottom,
                      static_cast<uint8_t>(std::min<uint16_t>(parameter(0, 1, true), TerminalScreen::ROWS)));
      break;
    case 'T':
      screen.scrollDown(scrollTop, scrollBottom,
                        static_cast<uint8_t>(std::min<uint16_t>(parameter(0, 1, true), TerminalScreen::ROWS)));
      break;
    case 'r': {
      const uint16_t top = parameter(0, 1, true);
      const uint16_t bottom = parameter(1, TerminalScreen::ROWS, true);
      if (top < bottom && bottom <= TerminalScreen::ROWS) {
        scrollTop = static_cast<uint8_t>(top - 1);
        scrollBottom = static_cast<uint8_t>(bottom - 1);
        screen.setCursor(1, 1);
      }
      break;
    }
    case 's':
      savedCursorRow = screen.getCursorRow();
      savedCursorColumn = screen.getCursorColumn();
      savedAttributes = screen.getAttributes();
      savedCursorValid = true;
      break;
    case 'u':
      if (savedCursorValid) {
        screen.setAttributes(savedAttributes);
        screen.setCursor(savedCursorRow + 1, savedCursorColumn + 1);
      }
      break;
    case 'm':
      dispatchSgr();
      break;
    default:
      break;
  }
}

void TerminalParser::dispatchSgr() {
  uint8_t attributes = screen.getAttributes();
  if (paramCount == 0) {
    screen.setAttributes(TerminalScreen::ATTR_NONE);
    return;
  }

  for (uint8_t index = 0; index < paramCount; ++index) {
    switch (params[index]) {
      case 0:
        attributes = TerminalScreen::ATTR_NONE;
        break;
      case 1:
        attributes |= TerminalScreen::ATTR_BOLD;
        break;
      case 4:
        attributes |= TerminalScreen::ATTR_UNDERLINE;
        break;
      case 7:
        attributes |= TerminalScreen::ATTR_INVERSE;
        break;
      case 22:
        attributes &= ~TerminalScreen::ATTR_BOLD;
        break;
      case 24:
        attributes &= ~TerminalScreen::ATTR_UNDERLINE;
        break;
      case 27:
        attributes &= ~TerminalScreen::ATTR_INVERSE;
        break;
      default:
        break;
    }
  }
  screen.setAttributes(attributes);
}
