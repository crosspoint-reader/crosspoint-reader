#include "AgentcloudProtocol.h"

#include <algorithm>
#include <cstring>

namespace agentcloud {
namespace {

constexpr uint8_t FRAME_START = 0x01;
constexpr uint8_t FRAME_CONTINUATION = 0x02;
constexpr uint8_t FRAME_FINAL = 0x03;
constexpr uint8_t MAX_JSON_DEPTH = 6;

class Parser {
 public:
  Parser(const char* begin, const char* end) : cursor(begin), end(end) {}

  bool parse(DashboardSnapshot& snapshot, ParseError& parseError) {
    output = &snapshot;
    error = ParseError::None;
    skipWhitespace();
    if (!consume('{')) return fail(ParseError::InvalidShape);

    uint8_t rootFields = 0;
    if (!peek('}')) {
      while (true) {
        char key[2]{};
        if (!parseString(key, sizeof(key))) return false;
        if (!consume(':')) return fail(ParseError::InvalidJson);

        uint8_t bit = 0;
        if (key[0] == 'u' && key[1] == '\0') {
          bit = 0x01;
          if (!parseRootCounter()) return false;
        } else if (key[0] == 'a' && key[1] == '\0') {
          bit = 0x02;
          if (!parseRootCounter()) return false;
        } else if (key[0] == 'r' && key[1] == '\0') {
          bit = 0x04;
          if (!parseRows()) return false;
        } else {
          return fail(ParseError::InvalidShape);
        }
        if ((rootFields & bit) != 0) return fail(ParseError::InvalidShape);
        rootFields |= bit;

        skipWhitespace();
        if (consume('}')) break;
        if (!consume(',')) return fail(ParseError::InvalidJson);
      }
    } else {
      ++cursor;
    }

    skipWhitespace();
    if (cursor != end) return fail(ParseError::InvalidJson);
    if (rootFields != 0x07 || rowCount != ROW_COUNT) return fail(ParseError::MissingRows);
    parseError = ParseError::None;
    return true;
  }

  ParseError getError() const { return error; }

 private:
  bool parseRows() {
    if (!consume('[')) return fail(ParseError::InvalidShape);
    skipWhitespace();
    if (consume(']')) return true;

    while (true) {
      if (rowCount >= ROW_COUNT) return fail(ParseError::TooManyRows);
      if (!parseRow(output->rows[rowCount])) return false;
      ++rowCount;
      skipWhitespace();
      if (consume(']')) return true;
      if (!consume(',')) return fail(ParseError::InvalidJson);
    }
  }

  bool parseRow(Card& card) {
    if (!consume('{')) return fail(ParseError::InvalidShape);
    uint16_t fields = 0;
    if (!peek('}')) {
      while (true) {
        char key[2]{};
        if (!parseString(key, sizeof(key))) return false;
        if (!consume(':')) return fail(ParseError::InvalidJson);

        uint16_t bit = 0;
        if (key[0] == 'i' && key[1] == '\0') {
          bit = 1u << 0;
          if (!parseString(card.id, sizeof(card.id))) return false;
        } else if (key[0] == 't' && key[1] == '\0') {
          bit = 1u << 1;
          if (!parseString(card.title, sizeof(card.title))) return false;
        } else if (key[0] == 'h' && key[1] == '\0') {
          bit = 1u << 2;
          if (!parseString(card.headline, sizeof(card.headline))) return false;
        } else if (key[0] == 'n' && key[1] == '\0') {
          bit = 1u << 3;
          if (!parseBool(card.unread)) return false;
        } else if (key[0] == 'l' && key[1] == '\0') {
          bit = 1u << 4;
          if (!parseBool(card.active)) return false;
        } else if (key[0] == 'q' && key[1] == '\0') {
          bit = 1u << 5;
          if (!parseBool(card.question)) return false;
        } else if (key[0] == 'd' && key[1] == '\0') {
          bit = 1u << 6;
          if (!parsePresence(card.recent, 2)) return false;
        } else if ((key[0] == 's' || key[0] == 'v' || key[0] == 'p') && key[1] == '\0') {
          bit = static_cast<uint16_t>(1u << (7 + (key[0] == 's' ? 0 : key[0] == 'v' ? 1 : 2)));
          if (!skipValue(2)) return false;
        } else {
          return fail(ParseError::InvalidShape);
        }
        if ((fields & bit) != 0) return fail(ParseError::InvalidShape);
        fields |= bit;

        skipWhitespace();
        if (consume('}')) break;
        if (!consume(',')) return fail(ParseError::InvalidJson);
      }
    } else {
      ++cursor;
    }

    if ((fields & (1u << 1)) == 0) return fail(ParseError::InvalidShape);
    if (card.title[0] != '\0' && card.id[0] == '\0') return fail(ParseError::MissingId);
    return true;
  }

  bool parseString(char* destination, size_t capacity) {
    skipWhitespace();
    if (cursor == end || *cursor++ != '"') return fail(ParseError::InvalidShape);
    size_t written = 0;
    while (cursor != end) {
      char value = *cursor++;
      if (value == '"') {
        if (destination) destination[written] = '\0';
        return true;
      }
      if (value == '\\') {
        if (cursor == end) return fail(ParseError::InvalidJson);
        const char escaped = *cursor++;
        if (escaped == '"' || escaped == '\\' || escaped == '/') {
          value = escaped;
        } else if (escaped == 'n' || escaped == 'r' || escaped == 't' || escaped == 'b' || escaped == 'f') {
          value = ' ';
        } else if (escaped == 'u') {
          uint16_t codepoint = 0;
          for (uint8_t i = 0; i < 4; ++i) {
            if (cursor == end) return fail(ParseError::InvalidJson);
            const char digit = *cursor++;
            codepoint <<= 4;
            if (digit >= '0' && digit <= '9') {
              codepoint |= static_cast<uint16_t>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
              codepoint |= static_cast<uint16_t>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
              codepoint |= static_cast<uint16_t>(digit - 'A' + 10);
            } else {
              return fail(ParseError::InvalidJson);
            }
          }
          value = (codepoint >= 0x20 && codepoint <= 0x7e) ? static_cast<char>(codepoint) : '?';
        } else {
          return fail(ParseError::InvalidJson);
        }
      }
      if (destination) {
        if (written + 1 >= capacity) return fail(ParseError::TextTooLong);
        destination[written] = value;
      }
      ++written;
    }
    return fail(ParseError::InvalidJson);
  }

  bool parseBool(bool& value) {
    skipWhitespace();
    if (matchLiteral("true")) {
      value = true;
      return true;
    }
    if (matchLiteral("false")) {
      value = false;
      return true;
    }
    return fail(ParseError::InvalidShape);
  }

  bool parseRootCounter() {
    skipWhitespace();
    if (cursor == end || *cursor < '0' || *cursor > '9') return fail(ParseError::InvalidShape);
    const char* start = cursor;
    uint32_t value = 0;
    do {
      const uint8_t digit = static_cast<uint8_t>(*cursor - '0');
      if (value > (UINT32_MAX - digit) / 10) return fail(ParseError::InvalidShape);
      value = value * 10 + digit;
      ++cursor;
    } while (cursor != end && *cursor >= '0' && *cursor <= '9');
    if (cursor - start > 1 && *start == '0') return fail(ParseError::InvalidShape);
    return true;
  }

  bool parsePresence(bool& present, uint8_t depth) {
    skipWhitespace();
    if (matchLiteral("null")) {
      present = false;
      return true;
    }
    present = true;
    return skipValue(depth);
  }

  bool skipValue(uint8_t depth) {
    if (depth > MAX_JSON_DEPTH) return fail(ParseError::TooDeep);
    skipWhitespace();
    if (cursor == end) return fail(ParseError::InvalidJson);
    if (*cursor == '"') return parseString(nullptr, 0);
    if (*cursor == '{') {
      ++cursor;
      skipWhitespace();
      if (consume('}')) return true;
      while (true) {
        if (!parseString(nullptr, 0) || !consume(':') || !skipValue(depth + 1)) return false;
        skipWhitespace();
        if (consume('}')) return true;
        if (!consume(',')) return fail(ParseError::InvalidJson);
      }
    }
    if (*cursor == '[') {
      ++cursor;
      skipWhitespace();
      if (consume(']')) return true;
      while (true) {
        if (!skipValue(depth + 1)) return false;
        skipWhitespace();
        if (consume(']')) return true;
        if (!consume(',')) return fail(ParseError::InvalidJson);
      }
    }
    if (matchLiteral("true") || matchLiteral("false") || matchLiteral("null")) return true;
    return parseNumber();
  }

  bool parseNumber() {
    skipWhitespace();
    const char* start = cursor;
    if (cursor != end && *cursor == '-') ++cursor;
    if (cursor == end) return fail(ParseError::InvalidJson);
    if (*cursor == '0') {
      ++cursor;
    } else if (*cursor >= '1' && *cursor <= '9') {
      do {
        ++cursor;
      } while (cursor != end && *cursor >= '0' && *cursor <= '9');
    } else {
      return fail(ParseError::InvalidJson);
    }
    if (cursor != end && *cursor == '.') {
      ++cursor;
      const char* digits = cursor;
      while (cursor != end && *cursor >= '0' && *cursor <= '9') ++cursor;
      if (cursor == digits) return fail(ParseError::InvalidJson);
    }
    if (cursor != end && (*cursor == 'e' || *cursor == 'E')) {
      ++cursor;
      if (cursor != end && (*cursor == '+' || *cursor == '-')) ++cursor;
      const char* digits = cursor;
      while (cursor != end && *cursor >= '0' && *cursor <= '9') ++cursor;
      if (cursor == digits) return fail(ParseError::InvalidJson);
    }
    return cursor != start;
  }

  bool matchLiteral(const char* literal) {
    const size_t length = strlen(literal);
    if (static_cast<size_t>(end - cursor) < length || memcmp(cursor, literal, length) != 0) return false;
    cursor += length;
    return true;
  }

  void skipWhitespace() {
    while (cursor != end && *cursor == ' ') ++cursor;
  }

  bool peek(char expected) {
    skipWhitespace();
    return cursor != end && *cursor == expected;
  }

  bool consume(char expected) {
    skipWhitespace();
    if (cursor == end || *cursor != expected) return false;
    ++cursor;
    return true;
  }

  bool fail(ParseError parseError) {
    if (error == ParseError::None) error = parseError;
    return false;
  }

  const char* cursor;
  const char* end;
  DashboardSnapshot* output = nullptr;
  ParseError error = ParseError::None;
  uint8_t rowCount = 0;
};

bool stringsEqual(const char* lhs, const char* rhs) { return strcmp(lhs, rhs) == 0; }

}  // namespace

void FrameAssembler::connect(const uint16_t newConnectionHandle) {
  reset();
  connectionHandle = newConnectionHandle;
}

void FrameAssembler::disconnect(const uint16_t disconnectedHandle) {
  if (connectionHandle == disconnectedHandle) {
    reset();
    connectionHandle = NO_CONNECTION;
  }
}

void FrameAssembler::reset() {
  payload.length = 0;
  payload.bytes[0] = '\0';
  authenticated = false;
}

FrameResult FrameAssembler::reject(const FrameResult result) {
  reset();
  return result;
}

bool FrameAssembler::append(const uint8_t* bytes, const size_t length) {
  if (length > MAX_PAYLOAD_BYTES - payload.length) return false;
  if (length != 0) memcpy(payload.bytes + payload.length, bytes, length);
  payload.length = static_cast<uint16_t>(payload.length + length);
  payload.bytes[payload.length] = '\0';
  return true;
}

FrameResult FrameAssembler::accept(const uint16_t frameConnection, const uint8_t* frame, const size_t length,
                                   const uint8_t authenticator[AUTH_BYTES]) {
  if (frameConnection != connectionHandle || connectionHandle == NO_CONNECTION) {
    return reject(FrameResult::WrongConnection);
  }
  if (frame == nullptr || length == 0 || length > 20) return reject(FrameResult::InvalidLength);

  if (frame[0] == FRAME_START) {
    reset();
    if (length < 1 + AUTH_BYTES) return reject(FrameResult::InvalidLength);
    if (memcmp(frame + 1, authenticator, AUTH_BYTES) != 0) return reject(FrameResult::InvalidAuth);
    authenticated = true;
    if (!append(frame + 1 + AUTH_BYTES, length - 1 - AUTH_BYTES)) return reject(FrameResult::Overflow);
    return FrameResult::Accepted;
  }

  if (frame[0] != FRAME_CONTINUATION && frame[0] != FRAME_FINAL) return reject(FrameResult::InvalidType);
  if (!authenticated) return reject(FrameResult::OutOfSequence);
  if (!append(frame + 1, length - 1)) return reject(FrameResult::Overflow);
  if (frame[0] == FRAME_FINAL) {
    authenticated = false;
    return FrameResult::Complete;
  }
  return FrameResult::Accepted;
}

bool parsePayload(const char* json, const size_t length, DashboardSnapshot& output, ParseError& error) {
  output = {};
  if (json == nullptr || length == 0) {
    error = ParseError::Empty;
    return false;
  }
  if (length > MAX_PAYLOAD_BYTES) {
    error = ParseError::TooLong;
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    const uint8_t byte = static_cast<uint8_t>(json[i]);
    if (byte < 0x20 || byte > 0x7e) {
      error = ParseError::NonPrintable;
      return false;
    }
  }

  Parser parser(json, json + length);
  if (!parser.parse(output, error)) {
    error = parser.getError();
    return false;
  }
  return true;
}

bool cardEquals(const Card& lhs, const Card& rhs) {
  return stringsEqual(lhs.id, rhs.id) && stringsEqual(lhs.title, rhs.title) &&
         stringsEqual(lhs.headline, rhs.headline) && lhs.unread == rhs.unread && lhs.active == rhs.active &&
         lhs.question == rhs.question && lhs.recent == rhs.recent;
}

bool snapshotEquals(const DashboardSnapshot& lhs, const DashboardSnapshot& rhs) { return dirtyRowMask(lhs, rhs) == 0; }

bool sameIdentity(const Card& lhs, const Card& rhs) {
  return lhs.id[0] != '\0' && rhs.id[0] != '\0' && stringsEqual(lhs.id, rhs.id);
}

bool hasNewSettledUnreadIdentity(const DashboardSnapshot& previous, const DashboardSnapshot& incoming) {
  for (const Card& incomingRow : incoming.rows) {
    if (incomingRow.title[0] == '\0' || incomingRow.id[0] == '\0' || !incomingRow.unread || incomingRow.active) {
      continue;
    }
    bool alreadySettledUnread = false;
    for (const Card& previousRow : previous.rows) {
      if (sameIdentity(previousRow, incomingRow) && previousRow.title[0] != '\0' && previousRow.unread &&
          !previousRow.active) {
        alreadySettledUnread = true;
        break;
      }
    }
    if (!alreadySettledUnread) return true;
  }
  return false;
}

uint8_t dirtyRowMask(const DashboardSnapshot& current, const DashboardSnapshot& incoming) {
  uint8_t mask = 0;
  for (size_t i = 0; i < ROW_COUNT; ++i) {
    if (!cardEquals(current.rows[i], incoming.rows[i])) mask |= static_cast<uint8_t>(1u << i);
  }
  return mask;
}

CardState cardState(const Card& card) {
  if (card.title[0] == '\0') return CardState::Empty;
  if (card.question) return CardState::Waiting;
  if (card.active) return CardState::Working;
  if (card.unread) return CardState::Unread;
  return CardState::Read;
}

uint8_t activeRowMask(const DashboardSnapshot& snapshot) {
  uint8_t mask = 0;
  for (size_t i = 0; i < ROW_COUNT; ++i) {
    if (cardState(snapshot.rows[i]) == CardState::Working) mask |= static_cast<uint8_t>(1u << i);
  }
  return mask;
}

uint8_t applyContentSpinnerStep(const uint8_t changedRows, const uint8_t previousActiveRows,
                                const uint8_t incomingActiveRows, const uint32_t nowMs, uint8_t& spinnerPhase,
                                uint32_t& lastSpinnerStepMs) {
  if (incomingActiveRows == 0) return changedRows;
  if (previousActiveRows == 0) {
    spinnerPhase = 0;
    lastSpinnerStepMs = nowMs;
    return changedRows;
  }
  if (!spinnerDue(lastSpinnerStepMs, nowMs, true)) return changedRows;
  spinnerPhase = static_cast<uint8_t>((spinnerPhase + 1) & 7);
  lastSpinnerStepMs = nowMs;
  return static_cast<uint8_t>(changedRows | incomingActiveRows);
}

bool isHighlighted(const Card& card) { return card.title[0] != '\0' && card.unread && !card.active; }

bool allRowsEmpty(const DashboardSnapshot& snapshot) {
  for (const Card& row : snapshot.rows) {
    if (row.title[0] != '\0') return false;
  }
  return true;
}

uint8_t bodyLineBudget(const uint8_t titleLines) {
  return titleLines <= 1 ? BODY_LINES_AFTER_ONE_TITLE : BODY_LINES_AFTER_TWO_TITLES;
}

bool spinnerDue(const uint32_t lastStepMs, const uint32_t nowMs, const bool hasActiveRow) {
  return hasActiveRow && static_cast<uint32_t>(nowMs - lastStepMs) >= SPINNER_INTERVAL_MS;
}

bool partialCleanupDue(const uint8_t partialRefreshCount) { return partialRefreshCount >= MAX_PARTIAL_REFRESHES; }

DashboardLayout makeLayout(const int screenWidth, const int screenHeight) {
  DashboardLayout layout{};
  if (screenWidth <= 0 || screenHeight <= 0) return layout;
  const int rowHeight = screenHeight / static_cast<int>(ROW_COUNT);
  for (size_t i = 0; i < ROW_COUNT; ++i) {
    const int top = static_cast<int>(i) * rowHeight;
    const int bottom = (i + 1 == ROW_COUNT) ? screenHeight : top + rowHeight;
    layout.rows[i] = {0, top, screenWidth, bottom - top};
  }
  return layout;
}

Rect dirtyBounds(const DashboardLayout& layout, const uint8_t dirtyMask) {
  int first = -1;
  int last = -1;
  for (size_t i = 0; i < ROW_COUNT; ++i) {
    if ((dirtyMask & (1u << i)) == 0) continue;
    if (first < 0) first = static_cast<int>(i);
    last = static_cast<int>(i);
  }
  if (first < 0) return {};
  const Rect& top = layout.rows[first];
  const Rect& bottom = layout.rows[last];
  return {top.x, top.y, top.width, bottom.y + bottom.height - top.y};
}

Rect stateIconRect(const Rect& rowRect) {
  return {rowRect.x + rowRect.width - ROW_PADDING - STATE_ICON_SIZE, rowRect.y + ROW_PADDING, STATE_ICON_SIZE,
          STATE_ICON_SIZE};
}

int separatorY(const size_t rowIndex, const Rect& rowRect) { return rowIndex == 0 ? -1 : rowRect.y; }

}  // namespace agentcloud
