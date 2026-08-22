#include "TerminalDiagnostics.h"

#include <algorithm>

namespace knietty::diagnostics {
namespace {

void writeU16(uint8_t*& output, const uint16_t value) {
  *output++ = static_cast<uint8_t>(value >> 8);
  *output++ = static_cast<uint8_t>(value);
}

void writeU32(uint8_t*& output, const uint32_t value) {
  *output++ = static_cast<uint8_t>(value >> 24);
  *output++ = static_cast<uint8_t>(value >> 16);
  *output++ = static_cast<uint8_t>(value >> 8);
  *output++ = static_cast<uint8_t>(value);
}

void fillRow(TerminalScreen& screen, const uint8_t row, const uint8_t variant) {
  screen.setCursor(row + 1, 1);
  screen.setAttributes(variant == 0 ? TerminalScreen::ATTR_NONE : TerminalScreen::ATTR_INVERSE);
  for (uint8_t column = 0; column < TerminalScreen::COLS; ++column) {
    screen.putCodepoint(static_cast<uint32_t>((column & 1) == variant ? '#' : ' '));
  }
  screen.carriageReturn();
}

void fillCells(TerminalScreen& screen, const uint8_t row, const uint8_t column, const uint8_t count,
               const uint8_t variant) {
  screen.setCursor(row + 1, column + 1);
  screen.setAttributes(variant == 0 ? TerminalScreen::ATTR_NONE : TerminalScreen::ATTR_INVERSE);
  for (uint8_t index = 0; index < count; ++index) screen.putCodepoint(' ');
  screen.setAttributes(TerminalScreen::ATTR_NONE);
}

uint8_t burstLength(const Pattern pattern) {
  switch (pattern) {
    case Pattern::Burst1:
      return 1;
    case Pattern::Burst2:
      return 2;
    case Pattern::Burst5:
      return 5;
    case Pattern::Burst10:
      return 10;
    case Pattern::Burst25:
      return 25;
    case Pattern::Burst100:
      return 100;
    default:
      return 0;
  }
}

}  // namespace

bool Request::causesRefresh() const {
  return command == Command::Reset || command == Command::Pattern || command == Command::SetPolarity ||
         command == Command::Clean;
}

Error decodeRequest(const uint8_t* payload, const size_t length, Request& request) {
  if (payload == nullptr || length == 0 || length > MAX_COMMAND_PAYLOAD) return Error::Malformed;
  const auto command = static_cast<Command>(payload[0]);
  request = {};
  request.command = command;
  switch (command) {
    case Command::SessionInfo:
    case Command::Reset:
    case Command::Clean:
    case Command::Stop:
    case Command::Metrics:
      return length == 1 ? Error::None : Error::Malformed;
    case Command::SetPolarity:
      if (length != 2) return Error::Malformed;
      if (payload[1] > 1) return Error::InvalidArgument;
      request.variant = payload[1];
      return Error::None;
    case Command::Pattern:
      if (length != 3) return Error::Malformed;
      if (payload[1] < static_cast<uint8_t>(Pattern::Cell) || payload[1] > static_cast<uint8_t>(Pattern::Burst100) ||
          payload[2] > 1) {
        return Error::InvalidArgument;
      }
      request.pattern = static_cast<Pattern>(payload[1]);
      request.variant = payload[2];
      return Error::None;
    default:
      return Error::UnknownCommand;
  }
}

bool isTerminalControlAllowed(const Request& request) {
  return request.command == Command::SessionInfo || request.command == Command::SetPolarity ||
         request.command == Command::Clean || request.command == Command::Metrics;
}

void applyRequest(TerminalScreen& screen, const Request& request) {
  if (request.command == Command::Reset) {
    screen.reset();
    screen.setCursorVisible(false);
    return;
  }
  if (request.command == Command::SetPolarity || request.command == Command::Clean) {
    screen.markAllDirty();
    return;
  }
  if (request.command != Command::Pattern) return;

  screen.setCursorVisible(false);
  screen.setAttributes(TerminalScreen::ATTR_NONE);
  switch (request.pattern) {
    case Pattern::Cell:
      fillCells(screen, 2, 2, 1, request.variant);
      break;
    case Pattern::Cursor:
      screen.setCursor(5, 7);
      screen.setCursorVisible(request.variant != 0);
      break;
    case Pattern::Row:
      fillRow(screen, 8, request.variant);
      break;
    case Pattern::DisjointRows:
      fillRow(screen, 4, request.variant);
      fillRow(screen, 18, request.variant);
      break;
    case Pattern::Scroll:
      screen.setCursor(TerminalScreen::ROWS, 1);
      for (uint8_t column = 0; column < TerminalScreen::COLS; ++column) {
        screen.putCodepoint(static_cast<uint32_t>(request.variant == 0 ? 'S' : 'T'));
      }
      screen.carriageReturn();
      screen.lineFeed();
      break;
    case Pattern::Checker:
      screen.reset();
      screen.setCursorVisible(false);
      for (uint8_t row = 0; row < TerminalScreen::ROWS; ++row) fillRow(screen, row, (row + request.variant) & 1);
      break;
    case Pattern::Full:
      screen.reset();
      screen.setCursorVisible(false);
      screen.setAttributes(request.variant == 0 ? TerminalScreen::ATTR_NONE : TerminalScreen::ATTR_INVERSE);
      for (uint16_t index = 0; index < static_cast<uint16_t>(TerminalScreen::ROWS) * TerminalScreen::COLS; ++index) {
        screen.putCodepoint('#');
      }
      break;
    case Pattern::CellMiddle:
      fillCells(screen, TerminalScreen::ROWS / 2, TerminalScreen::COLS / 2, 1, request.variant);
      break;
    case Pattern::CellBottom:
      fillCells(screen, TerminalScreen::ROWS - 2, TerminalScreen::COLS - 3, 1, request.variant);
      break;
    case Pattern::AdjacentCells:
      fillCells(screen, TerminalScreen::ROWS / 2, TerminalScreen::COLS / 2, 2, request.variant);
      break;
    case Pattern::BoundaryUnder:
      for (uint8_t row = 9; row < 13; ++row) fillRow(screen, row, request.variant);
      break;
    case Pattern::BoundaryOver:
      for (uint8_t row = 9; row < 14; ++row) fillRow(screen, row, request.variant);
      break;
    case Pattern::Burst1:
    case Pattern::Burst2:
    case Pattern::Burst5:
    case Pattern::Burst10:
    case Pattern::Burst25:
    case Pattern::Burst100:
      fillCells(screen, 11, 1, burstLength(request.pattern), request.variant);
      break;
  }
  screen.setAttributes(TerminalScreen::ATTR_NONE);
}

size_t encodeControlStatus(uint8_t* output, const size_t capacity, const Command command, const Status status,
                           const Error error) {
  if (output == nullptr || capacity < 4) return 0;
  output[0] = SCHEMA_VERSION;
  output[1] = static_cast<uint8_t>(command);
  output[2] = static_cast<uint8_t>(status);
  output[3] = static_cast<uint8_t>(error);
  return 4;
}

size_t encodeRefreshEvent(uint8_t* output, const size_t capacity, const RefreshEvent& event) {
  if (output == nullptr || capacity < REFRESH_EVENT_PAYLOAD_SIZE) return 0;
  uint8_t* cursor = output;
  *cursor++ = SCHEMA_VERSION;
  *cursor++ = static_cast<uint8_t>(event.phase);
  *cursor++ = static_cast<uint8_t>(event.command);
  *cursor++ = static_cast<uint8_t>(event.requestedPath);
  *cursor++ = static_cast<uint8_t>(event.actualPath);
  *cursor++ = static_cast<uint8_t>(event.fallbackReason);
  *cursor++ = event.flags;
  *cursor++ = event.queueDepth;
  writeU32(cursor, event.timestampUs);
  writeU32(cursor, event.rxAtUs);
  writeU32(cursor, event.parsedAtUs);
  writeU32(cursor, event.queuedAtUs);
  writeU32(cursor, event.renderStartedAtUs);
  writeU32(cursor, event.queueUs);
  writeU32(cursor, event.renderUs);
  writeU32(cursor, event.transferUs);
  writeU32(cursor, event.lutUs);
  writeU32(cursor, event.planeUs);
  writeU32(cursor, event.activationToBusyUs);
  writeU32(cursor, event.waveformUs);
  writeU32(cursor, event.baselineUs);
  writeU32(cursor, event.powerOffUs);
  writeU32(cursor, event.totalUs);
  writeU16(cursor, event.logicalX);
  writeU16(cursor, event.logicalY);
  writeU16(cursor, event.logicalWidth);
  writeU16(cursor, event.logicalHeight);
  writeU16(cursor, event.alignedX);
  writeU16(cursor, event.alignedY);
  writeU16(cursor, event.alignedWidth);
  writeU16(cursor, event.alignedHeight);
  writeU32(cursor, event.transferBytes);
  writeU16(cursor, event.dirtyCells);
  *cursor++ = event.dirtyRows;
  *cursor++ = event.coalesced;
  writeU32(cursor, event.firstSequence);
  writeU32(cursor, event.lastSequence);
  writeU32(cursor, event.freeHeap);
  writeU32(cursor, event.minimumFreeHeap);
  return static_cast<size_t>(cursor - output);
}

size_t encodeMetricsResponse(uint8_t* output, const size_t capacity, const MetricsSnapshot& metrics) {
  if (output == nullptr || capacity < METRICS_RESPONSE_PAYLOAD_SIZE) return 0;
  uint8_t* cursor = output;
  *cursor++ = SCHEMA_VERSION;
  *cursor++ = static_cast<uint8_t>(Command::Metrics);
  *cursor++ = static_cast<uint8_t>(Status::Accepted);
  *cursor++ = static_cast<uint8_t>(Error::None);
  writeU32(cursor, metrics.updates);
  writeU32(cursor, metrics.windowed);
  writeU32(cursor, metrics.fallback);
  writeU32(cursor, metrics.settle);
  writeU32(cursor, metrics.clean);
  writeU32(cursor, metrics.lastTotalUs);
  writeU32(cursor, metrics.lastWaveformUs);
  writeU32(cursor, metrics.lastQueueUs);
  writeU32(cursor, metrics.lastRenderUs);
  writeU32(cursor, metrics.lastTransferUs);
  writeU32(cursor, metrics.lastPlaneUs);
  writeU32(cursor, metrics.lastLutUs);
  writeU32(cursor, metrics.lastBaselineUs);
  writeU32(cursor, metrics.averageTotalUs);
  writeU32(cursor, metrics.minimumTotalUs);
  writeU32(cursor, metrics.maximumTotalUs);
  writeU16(cursor, metrics.lastRegionWidth);
  writeU16(cursor, metrics.lastRegionHeight);
  writeU32(cursor, metrics.lastRegionBytes);
  writeU32(cursor, metrics.freeHeap);
  writeU32(cursor, metrics.minimumFreeHeap);
  writeU32(cursor, metrics.rxBytes);
  writeU32(cursor, metrics.rxReads);
  writeU32(cursor, metrics.burstEnds);
  writeU32(cursor, metrics.burstSnapshots);
  writeU32(cursor, metrics.burstTimeouts);
  writeU32(cursor, metrics.asyncTailUpdates);
  return static_cast<size_t>(cursor - output);
}

}  // namespace knietty::diagnostics
