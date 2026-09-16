#pragma once

#include <cstddef>
#include <cstdint>

namespace agentcloud {

inline constexpr size_t AUTH_BYTES = 8;
inline constexpr size_t MAX_PAYLOAD_BYTES = 2047;
inline constexpr size_t ROW_COUNT = 4;
inline constexpr size_t MAX_ID_BYTES = 40;
inline constexpr size_t MAX_TITLE_BYTES = 40;
inline constexpr size_t MAX_HEADLINE_BYTES = 120;
inline constexpr uint8_t MAX_PARTIAL_REFRESHES = 60;
inline constexpr uint16_t NO_CONNECTION = UINT16_MAX;
inline constexpr int DASHBOARD_WIDTH = 480;
inline constexpr int DASHBOARD_HEIGHT = 800;
inline constexpr int ROW_HEIGHT = 200;
inline constexpr int ROW_PADDING = 27;
inline constexpr int STATE_ICON_SIZE = 34;
inline constexpr int STATE_ICON_GAP = 12;
inline constexpr int TITLE_LINE_HEIGHT = 40;
inline constexpr int TITLE_BODY_GAP = 2;
inline constexpr int BODY_LINE_HEIGHT = 30;
inline constexpr uint8_t TITLE_MAX_LINES = 2;
inline constexpr uint8_t BODY_LINES_AFTER_ONE_TITLE = 3;
inline constexpr uint8_t BODY_LINES_AFTER_TWO_TITLES = 2;

enum class FrameResult : uint8_t {
  Accepted,
  Complete,
  InvalidLength,
  InvalidType,
  InvalidAuth,
  WrongConnection,
  OutOfSequence,
  Overflow,
};

struct PayloadMessage {
  uint16_t length = 0;
  char bytes[MAX_PAYLOAD_BYTES + 1]{};
};

class FrameAssembler {
 public:
  void connect(uint16_t connectionHandle);
  void disconnect(uint16_t connectionHandle);
  void reset();
  FrameResult accept(uint16_t connectionHandle, const uint8_t* frame, size_t length,
                     const uint8_t authenticator[AUTH_BYTES]);

  const PayloadMessage& message() const { return payload; }
  uint16_t connection() const { return connectionHandle; }

 private:
  FrameResult reject(FrameResult result);
  bool append(const uint8_t* bytes, size_t length);

  PayloadMessage payload{};
  uint16_t connectionHandle = NO_CONNECTION;
  bool authenticated = false;
};

enum class ParseError : uint8_t {
  None,
  Empty,
  TooLong,
  NonPrintable,
  InvalidJson,
  InvalidShape,
  TooManyRows,
  MissingRows,
  MissingId,
  TextTooLong,
  TooDeep,
};

enum class CardState : uint8_t { Empty, Unread, Read, Waiting, Working };
enum class GrayscaleRefreshPolicy : uint8_t { Unavailable, Fast, Half, Full };

struct Card {
  char id[MAX_ID_BYTES + 1]{};
  char title[MAX_TITLE_BYTES + 1]{};
  char headline[MAX_HEADLINE_BYTES + 1]{};
  bool unread = false;
  bool active = false;
  bool question = false;
  bool recent = false;
};

struct DashboardSnapshot {
  Card rows[ROW_COUNT]{};
};

bool parsePayload(const char* json, size_t length, DashboardSnapshot& output, ParseError& error);
bool cardEquals(const Card& lhs, const Card& rhs);
bool snapshotEquals(const DashboardSnapshot& lhs, const DashboardSnapshot& rhs);
bool sameIdentity(const Card& lhs, const Card& rhs);
bool hasNewSettledUnreadIdentity(const DashboardSnapshot& previous, const DashboardSnapshot& incoming);
uint8_t dirtyRowMask(const DashboardSnapshot& current, const DashboardSnapshot& incoming);
uint8_t activeRowMask(const DashboardSnapshot& snapshot);
CardState cardState(const Card& card);
bool isHighlighted(const Card& card);
bool allRowsEmpty(const DashboardSnapshot& snapshot);
uint8_t bodyLineBudget(uint8_t titleLines);
constexpr bool textAntiAliasingAvailable(const bool settingEnabled, const bool supported, const bool stripUploads) {
  return settingEnabled && supported && stripUploads;
}
constexpr GrayscaleRefreshPolicy grayscaleRefreshPolicy(const bool available, const bool firstPaint,
                                                        const bool forceFullRefresh) {
  if (!available) return GrayscaleRefreshPolicy::Unavailable;
  if (forceFullRefresh) return GrayscaleRefreshPolicy::Full;
  return firstPaint ? GrayscaleRefreshPolicy::Half : GrayscaleRefreshPolicy::Fast;
}
bool partialCleanupDue(uint8_t partialRefreshCount);

struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct DashboardLayout {
  Rect rows[ROW_COUNT]{};
};

DashboardLayout makeLayout(int screenWidth, int screenHeight);
Rect dirtyBounds(const DashboardLayout& layout, uint8_t dirtyMask);
Rect stateIconRect(const Rect& rowRect);
int separatorY(size_t rowIndex, const Rect& rowRect);

static_assert(sizeof(PayloadMessage) <= 2052, "BLE queue item must stay bounded");
static_assert(sizeof(DashboardSnapshot) <= 840, "dashboard snapshot must stay bounded");
static_assert(DASHBOARD_HEIGHT == static_cast<int>(ROW_COUNT) * ROW_HEIGHT, "dashboard rows must fill the panel");

}  // namespace agentcloud
