#include <cstdint>
#include <cstdio>
#include <cstring>

#include "AgentcloudProtocol.h"
#include "AgentcloudMaterialIcons.h"
#include "DisplayWindowGeometry.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                               \
    }                                                                           \
  } while (false)

constexpr uint8_t AUTH[agentcloud::AUTH_BYTES] = {1, 2, 3, 4, 5, 6, 7, 8};
constexpr char LIVE_PAYLOAD[] =
    R"({"u":2,"a":1,"r":[{"i":"one","t":"Build","h":"Compiling","n":true,"l":true,"q":false,"d":null,"s":1,"v":2,"p":3},{"i":"two","t":"Review","h":"Needs input","n":true,"l":true,"q":true,"d":null},{"i":"three","t":"Done","h":"Ready","n":true,"l":false,"q":false,"d":null},{"t":""}]})";

void testFrames() {
  agentcloud::FrameAssembler assembler;
  assembler.connect(7);
  uint8_t start[20] = {0x01};
  memcpy(start + 1, AUTH, sizeof(AUTH));
  memcpy(start + 9, "{\"u\":0,", 7);
  CHECK(assembler.accept(7, start, 16, AUTH) == agentcloud::FrameResult::Accepted);
  const uint8_t continuation[] = {0x02, '"', 'a', '"', ':', '0', ','};
  CHECK(assembler.accept(7, continuation, sizeof(continuation), AUTH) == agentcloud::FrameResult::Accepted);
  const uint8_t final[] = {0x03, '"', 'r', '"', ':', '[', ']', '}'};
  CHECK(assembler.accept(7, final, sizeof(final), AUTH) == agentcloud::FrameResult::Complete);
  CHECK(strcmp(assembler.message().bytes, "{\"u\":0,\"a\":0,\"r\":[]}") == 0);

  assembler.connect(7);
  CHECK(assembler.accept(8, continuation, sizeof(continuation), AUTH) == agentcloud::FrameResult::WrongConnection);
  CHECK(assembler.accept(7, continuation, sizeof(continuation), AUTH) == agentcloud::FrameResult::OutOfSequence);
  uint8_t badAuth[10] = {0x01};
  CHECK(assembler.accept(7, badAuth, sizeof(badAuth), AUTH) == agentcloud::FrameResult::InvalidAuth);

  assembler.connect(7);
  memcpy(start + 1, AUTH, sizeof(AUTH));
  memcpy(start + 9, "12345678901", 11);
  CHECK(assembler.accept(7, start, sizeof(start), AUTH) == agentcloud::FrameResult::Accepted);
  uint8_t full[20] = {0x02};
  memset(full + 1, 'x', sizeof(full) - 1);
  for (size_t i = 0; i < 107; ++i)
    CHECK(assembler.accept(7, full, sizeof(full), AUTH) == agentcloud::FrameResult::Accepted);
  CHECK(assembler.accept(7, full, sizeof(full), AUTH) == agentcloud::FrameResult::Overflow);

  assembler.connect(9);
  CHECK(assembler.accept(9, start, sizeof(start), AUTH) == agentcloud::FrameResult::Accepted);
  uint8_t tooLarge[21] = {0x02};
  CHECK(assembler.accept(9, tooLarge, sizeof(tooLarge), AUTH) == agentcloud::FrameResult::InvalidLength);
}

agentcloud::DashboardSnapshot parseLive() {
  agentcloud::DashboardSnapshot snapshot{};
  agentcloud::ParseError error = agentcloud::ParseError::None;
  CHECK(agentcloud::parsePayload(LIVE_PAYLOAD, strlen(LIVE_PAYLOAD), snapshot, error));
  CHECK(error == agentcloud::ParseError::None);
  return snapshot;
}

void testPayload() {
  auto snapshot = parseLive();
  CHECK(strcmp(snapshot.rows[0].id, "one") == 0);
  CHECK(strcmp(snapshot.rows[0].title, "Build") == 0);
  CHECK(strcmp(snapshot.rows[0].headline, "Compiling") == 0);
  CHECK(agentcloud::cardState(snapshot.rows[0]) == agentcloud::CardState::Working);
  CHECK(!agentcloud::isHighlighted(snapshot.rows[0]));
  CHECK(agentcloud::cardState(snapshot.rows[1]) == agentcloud::CardState::Waiting);
  CHECK(agentcloud::cardState(snapshot.rows[2]) == agentcloud::CardState::Unread);
  CHECK(agentcloud::isHighlighted(snapshot.rows[2]));
  CHECK(!snapshot.rows[2].recent);
  CHECK(agentcloud::cardState(snapshot.rows[3]) == agentcloud::CardState::Empty);

  constexpr char readRecentPayload[] =
      R"({"u":0,"a":0,"r":[{"i":"read","t":"Read","h":"Seen","n":false,"l":false,"q":false,"d":123},{"t":""},{"t":""},{"t":""}]})";
  agentcloud::DashboardSnapshot readSnapshot{};
  agentcloud::ParseError readError = agentcloud::ParseError::None;
  CHECK(agentcloud::parsePayload(readRecentPayload, strlen(readRecentPayload), readSnapshot, readError));
  CHECK(readSnapshot.rows[0].recent);
  CHECK(agentcloud::cardState(readSnapshot.rows[0]) == agentcloud::CardState::Read);
  CHECK(!agentcloud::isHighlighted(readSnapshot.rows[0]));

  agentcloud::Card priority = readSnapshot.rows[0];
  priority.unread = true;
  CHECK(agentcloud::cardState(priority) == agentcloud::CardState::Unread);
  priority.active = true;
  CHECK(agentcloud::cardState(priority) == agentcloud::CardState::Working);
  priority.question = true;
  CHECK(agentcloud::cardState(priority) == agentcloud::CardState::Waiting);

  agentcloud::DashboardSnapshot rejected{};
  agentcloud::ParseError error = agentcloud::ParseError::None;
  CHECK(!agentcloud::parsePayload("{", 1, rejected, error));
  constexpr char control[] = "{\"u\":0,\"a\":0,\"r\":[]}\n";
  CHECK(!agentcloud::parsePayload(control, sizeof(control) - 1, rejected, error));
  constexpr char threeRows[] = R"({"u":0,"a":0,"r":[{"t":""},{"t":""},{"t":""}]})";
  CHECK(!agentcloud::parsePayload(threeRows, strlen(threeRows), rejected, error));
  constexpr char fiveRows[] = R"({"u":0,"a":0,"r":[{"t":""},{"t":""},{"t":""},{"t":""},{"t":""}]})";
  CHECK(!agentcloud::parsePayload(fiveRows, strlen(fiveRows), rejected, error));
  constexpr char missingId[] = R"({"u":0,"a":0,"r":[{"t":"live"},{"t":""},{"t":""},{"t":""}]})";
  CHECK(!agentcloud::parsePayload(missingId, strlen(missingId), rejected, error));
  constexpr char trailing[] = R"({"u":0,"a":0,"r":[{"t":""},{"t":""},{"t":""},{"t":""}]}x)";
  CHECK(!agentcloud::parsePayload(trailing, strlen(trailing), rejected, error));
  constexpr char maxCounters[] =
      R"({"u":4294967295,"a":4294967295,"r":[{"t":""},{"t":""},{"t":""},{"t":""}]})";
  CHECK(agentcloud::parsePayload(maxCounters, strlen(maxCounters), rejected, error));
  constexpr char negativeCounter[] = R"({"u":-1,"a":0,"r":[{"t":""},{"t":""},{"t":""},{"t":""}]})";
  CHECK(!agentcloud::parsePayload(negativeCounter, strlen(negativeCounter), rejected, error));
  constexpr char fractionalCounter[] =
      R"({"u":1.5,"a":0,"r":[{"t":""},{"t":""},{"t":""},{"t":""}]})";
  CHECK(!agentcloud::parsePayload(fractionalCounter, strlen(fractionalCounter), rejected, error));
  constexpr char overflowCounter[] =
      R"({"u":4294967296,"a":0,"r":[{"t":""},{"t":""},{"t":""},{"t":""}]})";
  CHECK(!agentcloud::parsePayload(overflowCounter, strlen(overflowCounter), rejected, error));
  constexpr char typedCounter[] = R"({"u":"1","a":0,"r":[{"t":""},{"t":""},{"t":""},{"t":""}]})";
  CHECK(!agentcloud::parsePayload(typedCounter, strlen(typedCounter), rejected, error));
  constexpr char leadingZeroUnread[] = R"({"u":01,"a":0,"r":[{"t":""},{"t":""},{"t":""},{"t":""}]})";
  CHECK(!agentcloud::parsePayload(leadingZeroUnread, strlen(leadingZeroUnread), rejected, error));
  constexpr char leadingZeroActive[] = R"({"u":0,"a":00,"r":[{"t":""},{"t":""},{"t":""},{"t":""}]})";
  CHECK(!agentcloud::parsePayload(leadingZeroActive, strlen(leadingZeroActive), rejected, error));
}

void testUpdates() {
  auto before = parseLive();
  auto after = before;
  memcpy(after.rows[0].title, "Renamed", 8);
  CHECK(agentcloud::sameIdentity(before.rows[0], after.rows[0]));
  CHECK(agentcloud::dirtyRowMask(before, after) == 0x01);
  CHECK(!agentcloud::snapshotEquals(before, after));
  CHECK(agentcloud::snapshotEquals(after, after));
  CHECK(agentcloud::activeRowMask(before) == 0x01);
  auto idleRowChanged = before;
  memcpy(idleRowChanged.rows[2].headline, "New", 4);
  CHECK(agentcloud::dirtyRowMask(before, idleRowChanged) == 0x04);
  uint8_t phase = 3;
  uint32_t deadlineBase = 1000;
  CHECK(agentcloud::applyContentSpinnerStep(0x04, 0x01, 0x01, 5000, phase, deadlineBase) == 0x04);
  CHECK(phase == 3);
  CHECK(deadlineBase == 1000);
  CHECK(agentcloud::applyContentSpinnerStep(0x08, 0x01, 0x01, 15999, phase, deadlineBase) == 0x08);
  CHECK(phase == 3);
  CHECK(deadlineBase == 1000);
  CHECK(agentcloud::applyContentSpinnerStep(0x04, 0x01, 0x01, 16000, phase, deadlineBase) == 0x05);
  CHECK(phase == 4);
  CHECK(deadlineBase == 16000);
  CHECK(agentcloud::applyContentSpinnerStep(0x08, 0x01, 0x01, 20000, phase, deadlineBase) == 0x08);
  CHECK(phase == 4);
  CHECK(deadlineBase == 16000);
  phase = 6;
  deadlineBase = 0;
  CHECK(agentcloud::applyContentSpinnerStep(0x01, 0, 0x01, 500, phase, deadlineBase) == 0x01);
  CHECK(phase == 0);
  CHECK(deadlineBase == 500);
  CHECK(!agentcloud::spinnerDue(1000, 15999, true));
  CHECK(agentcloud::spinnerDue(1000, 16000, true));
  CHECK(!agentcloud::spinnerDue(1000, 20000, false));
  CHECK(!agentcloud::partialCleanupDue(59));
  CHECK(agentcloud::partialCleanupDue(60));
}

void testGeometry() {
  enum class Orientation : uint8_t { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  for (uint8_t value = 0; value < 4; ++value) {
    const auto orientation = static_cast<Orientation>(value);
    const bool portrait = value == 0 || value == 2;
    const auto layout = agentcloud::makeLayout(portrait ? 480 : 800, portrait ? 800 : 480);
    CHECK(layout.rows[0].y == 0);
    CHECK(layout.rows[3].y + layout.rows[3].height == (portrait ? 800 : 480));
    if (portrait) CHECK(layout.rows[0].height == 200);
    for (size_t i = 0; i < agentcloud::ROW_COUNT; ++i) {
      const auto& row = layout.rows[i];
      const auto physical =
          display_window::screenRectToAlignedMemRect(orientation, row.x, row.y, row.width, row.height, 800, 480);
      CHECK(physical.valid);
      CHECK(physical.x % 8 == 0);
      CHECK(physical.w % 8 == 0);
      CHECK(physical.x + physical.w <= 800);
      CHECK(physical.y + physical.h <= 480);
      if (i != 0) CHECK(layout.rows[i - 1].y + layout.rows[i - 1].height == row.y);
    }
    const auto bounds = agentcloud::dirtyBounds(layout, 0x06);
    CHECK(bounds.y == layout.rows[1].y);
    CHECK(bounds.height == layout.rows[2].y + layout.rows[2].height - layout.rows[1].y);
  }

  const auto portraitLayout = agentcloud::makeLayout(agentcloud::DASHBOARD_WIDTH, agentcloud::DASHBOARD_HEIGHT);
  for (size_t i = 0; i < agentcloud::ROW_COUNT; ++i) {
    CHECK(portraitLayout.rows[i].x == 0);
    CHECK(portraitLayout.rows[i].y == static_cast<int>(i) * agentcloud::ROW_HEIGHT);
    CHECK(portraitLayout.rows[i].width == agentcloud::DASHBOARD_WIDTH);
    CHECK(portraitLayout.rows[i].height == agentcloud::ROW_HEIGHT);
    const auto icon = agentcloud::stateIconRect(portraitLayout.rows[i]);
    CHECK(icon.x == 419);
    CHECK(icon.y == static_cast<int>(i) * agentcloud::ROW_HEIGHT + 27);
    CHECK(icon.width == 34);
    CHECK(icon.height == 34);
    CHECK(icon.x >= portraitLayout.rows[i].x);
    CHECK(icon.y >= portraitLayout.rows[i].y);
    CHECK(icon.x + icon.width <= portraitLayout.rows[i].x + portraitLayout.rows[i].width);
    CHECK(icon.y + icon.height <= portraitLayout.rows[i].y + portraitLayout.rows[i].height);
    CHECK(agentcloud::separatorY(i, portraitLayout.rows[i]) ==
          (i == 0 ? -1 : static_cast<int>(i) * agentcloud::ROW_HEIGHT));
  }
  CHECK(agentcloud::TITLE_MAX_LINES == 2);
  CHECK(agentcloud::TITLE_LINE_HEIGHT == 40);
  CHECK(agentcloud::TITLE_BODY_GAP == 2);
  CHECK(agentcloud::BODY_LINE_HEIGHT == 30);
  CHECK(agentcloud::bodyLineBudget(1) == 3);
  CHECK(agentcloud::bodyLineBudget(2) == 2);
}

void testPresentationContracts() {
  agentcloud::DashboardSnapshot snapshot{};
  CHECK(agentcloud::allRowsEmpty(snapshot));
  memcpy(snapshot.rows[2].title, "Visible", 8);
  CHECK(!agentcloud::allRowsEmpty(snapshot));
  CHECK(agentcloud::isHighlighted(snapshot.rows[2]) == false);
  snapshot.rows[2].unread = true;
  CHECK(agentcloud::isHighlighted(snapshot.rows[2]));
  snapshot.rows[2].active = true;
  CHECK(!agentcloud::isHighlighted(snapshot.rows[2]));

  agentcloud::DashboardSnapshot empty{};
  agentcloud::DashboardSnapshot settled{};
  memcpy(settled.rows[0].id, "stable", 7);
  memcpy(settled.rows[0].title, "Original", 9);
  settled.rows[0].unread = true;
  CHECK(agentcloud::hasNewSettledUnreadIdentity(empty, settled));
  CHECK(!agentcloud::hasNewSettledUnreadIdentity(settled, settled));

  auto renamedAndReordered = settled;
  renamedAndReordered.rows[0] = {};
  renamedAndReordered.rows[2] = settled.rows[0];
  memcpy(renamedAndReordered.rows[2].title, "Renamed", 8);
  CHECK(!agentcloud::hasNewSettledUnreadIdentity(settled, renamedAndReordered));

  auto activeUnread = settled;
  activeUnread.rows[0].active = true;
  CHECK(!agentcloud::hasNewSettledUnreadIdentity(empty, activeUnread));
  CHECK(agentcloud::hasNewSettledUnreadIdentity(activeUnread, settled));

  auto missingStableId = settled;
  missingStableId.rows[0].id[0] = '\0';
  CHECK(!agentcloud::hasNewSettledUnreadIdentity(empty, missingStableId));

  auto readRecent = settled;
  readRecent.rows[0].unread = false;
  readRecent.rows[0].recent = true;
  CHECK(!agentcloud::hasNewSettledUnreadIdentity(empty, readRecent));

  CHECK(STATE_ICON_BYTES == 170);
  CHECK(STATE_ICON_IMAGE_COUNT == 11);
  CHECK(sizeof(state_icon_unread_bits) == STATE_ICON_BYTES);
  CHECK(sizeof(state_icon_read_bits) == STATE_ICON_BYTES);
  CHECK(sizeof(state_icon_question_bits) == STATE_ICON_BYTES);
  CHECK(sizeof(state_icon_working_0_bits) == STATE_ICON_BYTES);
  CHECK(sizeof(state_icon_working_1_bits) == STATE_ICON_BYTES);
  CHECK(sizeof(state_icon_working_2_bits) == STATE_ICON_BYTES);
  CHECK(sizeof(state_icon_working_3_bits) == STATE_ICON_BYTES);
  CHECK(sizeof(state_icon_working_4_bits) == STATE_ICON_BYTES);
  CHECK(sizeof(state_icon_working_5_bits) == STATE_ICON_BYTES);
  CHECK(sizeof(state_icon_working_6_bits) == STATE_ICON_BYTES);
  CHECK(sizeof(state_icon_working_7_bits) == STATE_ICON_BYTES);
  CHECK(sizeof(state_icon_working_frames) / sizeof(state_icon_working_frames[0]) == 8);
}

}  // namespace

int main() {
  testFrames();
  testPayload();
  testUpdates();
  testGeometry();
  testPresentationContracts();
  if (failures != 0) return 1;
  std::puts("agentcloud protocol tests passed");
  return 0;
}
