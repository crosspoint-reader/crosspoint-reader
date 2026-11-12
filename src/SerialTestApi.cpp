#include "SerialTestApi.h"

#if ENABLE_SERIAL_CONTROL

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>

#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "util/ButtonPressQueue.h"
#include "util/IdleGate.h"
#include "util/ScreenshotInfo.h"
#include "util/SerialCommandGrammar.h"

namespace SerialTestApi {

namespace {

using SerialCommandGrammar::ReplyLine;

// ---- Output ---------------------------------------------------------------------------------

constexpr unsigned long REPLY_STALL_MS = 2000;
constexpr unsigned long FRAME_STALL_MS = 15000;

// logSerial.write() returns short while the USB TX buffer stays full past its 1 ms timeout (set in
// setup()), so this loops until the host has taken everything. It yields between tries: a busy
// loop on the C3 starves the USB task it waits for. False after `stallMs` without progress.
bool writeAll(const uint8_t* data, const size_t len, const unsigned long stallMs) {
  size_t sent = 0;
  unsigned long lastProgressMs = millis();
  while (sent < len) {
    const size_t n = logSerial.write(data + sent, len - sent);
    if (n > 0) {
      sent += n;
      lastProgressMs = millis();
      continue;
    }
    if (millis() - lastProgressMs > stallMs) return false;
    delay(1);
  }
  return true;
}

// Replies are built here, one at a time on the main loop task. One byte stays free for the '\n'.
char replyBuffer[320];

ReplyLine startReply(const std::string_view head) {
  ReplyLine line(replyBuffer, sizeof(replyBuffer) - 1);
  line.append(head);
  return line;
}

// Output given up part way (a reply, or a frame) leaves its line open: the next reply first ends it,
// so it arrives on a line of its own.
bool lineLeftOpen = false;

void send(const ReplyLine& line) {
  static constexpr uint8_t LINE_END = '\n';
  if (lineLeftOpen && writeAll(&LINE_END, 1, REPLY_STALL_MS)) lineLeftOpen = false;
  const size_t length = line.view().size();
  replyBuffer[length] = '\n';
  if (!writeAll(reinterpret_cast<const uint8_t*>(replyBuffer), length + 1, REPLY_STALL_MS)) lineLeftOpen = true;
}

void reply(const std::string_view head, const std::string_view tail = {}) {
  ReplyLine line = startReply(head);
  line.append(tail);
  send(line);
}

void replyNumber(const std::string_view head, const unsigned long value) {
  char number[16];
  const int n = snprintf(number, sizeof(number), "%lu", value);
  reply(head, std::string_view(number, n > 0 ? static_cast<size_t>(n) : 0));
}

// ---- State shared by the commands and service() ---------------------------------------------

GfxRenderer* screen = nullptr;  // set by poll()
ButtonPressQueue presses;
uint8_t heldButtons = 0;
IdleGate idleGate;
constexpr int IDLE_DEFAULT_TIMEOUT_MS = 10000;
constexpr int IDLE_MAX_TIMEOUT_MS = 600000;

// ---- BTN ------------------------------------------------------------------------------------

struct NamedButton {
  const char* name;
  uint8_t index;
};
// The SDK's button bits (HalGPIO::BTN_*): the user's button mapping applies downstream, as for a key.
constexpr NamedButton BUTTONS[] = {
    {"BACK", HalGPIO::BTN_BACK},   {"CONFIRM", HalGPIO::BTN_CONFIRM}, {"LEFT", HalGPIO::BTN_LEFT},
    {"RIGHT", HalGPIO::BTN_RIGHT}, {"UP", HalGPIO::BTN_UP},           {"DOWN", HalGPIO::BTN_DOWN},
    {"POWER", HalGPIO::BTN_POWER},
};

void cmdButton(const std::string_view arg) {
  using SerialCommandGrammar::PressKind;
  std::string_view name;
  PressKind kind = PressKind::Short;
  int holdMs = 0;
  if (!SerialCommandGrammar::parseButtonArg(arg, name, kind, holdMs, ButtonPressQueue::MAX_HOLD_MS)) {
    reply("BTN_ERROR:bad press ", arg);
    return;
  }
  const auto* button =
      std::find_if(std::begin(BUTTONS), std::end(BUTTONS), [&](const NamedButton& b) { return name == b.name; });
  if (button == std::end(BUTTONS)) {
    reply("BTN_ERROR:unknown button ", name);
    return;
  }
  // Releasing nothing: a no-op that tells whether this board lets the hook in.
  if (presses.idle() && !gpio.holdSyntheticButtons(0)) {
    reply("BTN_ERROR:not supported on this board");
    return;
  }
  const size_t needed = kind == PressKind::Double ? 2 : 1;
  if (presses.freeSlots() < needed) {
    reply("BTN_ERROR:busy");
    return;
  }
  uint16_t hold = ButtonPressQueue::SHORT_HOLD_MS;
  if (kind == PressKind::Long) hold = ButtonPressQueue::LONG_HOLD_MS;
  if (kind == PressKind::Hold) hold = static_cast<uint16_t>(holdMs);
  presses.push(button->index, hold);
  if (kind == PressKind::Double) presses.push(button->index, hold);
  idleGate.noteEffect(presses.takenPresses());
  reply("BTN_OK");
}

// ---- ACT ------------------------------------------------------------------------------------

void cmdActivity(const std::string_view arg) {
  std::string_view target;
  std::string_view rest;
  SerialCommandGrammar::splitHead(arg, target, rest);
  if (target == "HOME") {
    activityManager.goHome();
  } else if (target == "READER") {
    std::string path(rest);
    if (path.empty() || !Storage.exists(path.c_str())) {
      reply("ACT_ERROR:no such file ", rest);
      return;
    }
    activityManager.goToReader(std::move(path));
  } else if (target == "FILES") {
    activityManager.goToFileBrowser(std::string(rest));  // empty: the card's top folder
  } else if (target == "LIBRARY") {
    activityManager.goToLibrary();
  } else if (target == "SETTINGS") {
    activityManager.goToSettings();
  } else {
    reply("ACT_ERROR:unknown screen ", target);
    return;
  }
  // A screen that could not be created (a book the reader refuses, no memory) queues no change.
  if (!activityManager.hasPendingTransition()) {
    reply("ACT_ERROR:cannot open ", arg);
    return;
  }
  reply("ACT_OK");
}

// ---- STATE ----------------------------------------------------------------------------------

const char* readerTypeName(const ScreenshotInfo::ReaderType type) {
  switch (type) {
    case ScreenshotInfo::ReaderType::None:
      return "none";
    case ScreenshotInfo::ReaderType::Epub:
      return "epub";
    case ScreenshotInfo::ReaderType::Txt:
      return "txt";
    case ScreenshotInfo::ReaderType::Xtc:
      return "xtc";
  }
  return "none";
}

const char* orientationName(const GfxRenderer::Orientation orientation) {
  switch (orientation) {
    case GfxRenderer::Portrait:
      return "portrait";
    case GfxRenderer::LandscapeClockwise:
      return "landscape_cw";
    case GfxRenderer::PortraitInverted:
      return "portrait_inverted";
    case GfxRenderer::LandscapeCounterClockwise:
      return "landscape_ccw";
  }
  return "portrait";
}

void cmdState(std::string_view /*arg*/) {
  ReplyLine line = startReply("STATE:");
  line.appendField("activity", activityManager.currentActivityName());
  ScreenshotInfo info;
  {
    RenderLock lock;  // the reader's position is also written by the render task
    info = activityManager.getScreenshotInfo();
    if (info.readerType != ScreenshotInfo::ReaderType::None) {
      line.appendField("reader", readerTypeName(info.readerType));
      if (info.spineIndex >= 0) line.appendField("spine", static_cast<long>(info.spineIndex + 1));
      line.appendField("page", static_cast<long>(info.currentPage));
      line.appendField("pages", static_cast<long>(info.totalPages));
      line.appendField("progress", static_cast<long>(info.progressPercent));
    }
    line.appendField("orientation", orientationName(screen->getOrientation()));
  }
  line.appendField("visit", static_cast<long>(activityManager.visit()));
  const HalMemory::HeapStats heap = HalMemory::getInternalHeap();
  line.appendField("heap", static_cast<long>(heap.freeBytes));
  line.appendField("maxblock", static_cast<long>(heap.largestBlockBytes));
  line.appendField("minheap", static_cast<long>(heap.minFreeBytes));
  line.appendField("uptime", static_cast<long>(millis()));
  if (presses.droppedPresses() > 0) line.appendField("btn_dropped", static_cast<long>(presses.droppedPresses()));

  // The free text goes last, cut to the room left, so the fields above always arrive whole.
  char names[128];
  ReplyLine stack(names, sizeof(names));
  for (size_t i = 0; i < activityManager.stackedActivityCount(); ++i) {
    if (i > 0) stack.append(",");
    stack.append(activityManager.stackedActivityName(i));
  }
  SerialCommandGrammar::TailField tail[] = {{"stack", stack.view(), stack.truncated()},
                                            {"title", std::string_view(info.title)}};
  const size_t tailCount = info.readerType != ScreenshotInfo::ReaderType::None ? 2 : 1;
  if (!SerialCommandGrammar::appendClippedTail(line, tail, tailCount)) {
    reply("STATE_ERROR:reply too long");
    return;
  }
  send(line);
}

// ---- IDLE -----------------------------------------------------------------------------------

void cmdIdle(const std::string_view arg) {
  int timeout = IDLE_DEFAULT_TIMEOUT_MS;
  if (!arg.empty() && (!SerialCommandGrammar::parseInt(arg, timeout) || timeout <= 0)) {
    reply("IDLE_ERROR:bad timeout ", arg);
    return;
  }
  // Answered by service() once the device is idle or the time is up. One wait at a time, so that
  // each IDLE gets its own answer.
  if (!idleGate.begin(millis(), static_cast<uint32_t>(std::min(timeout, IDLE_MAX_TIMEOUT_MS)))) {
    reply("IDLE_ERROR:busy");
  }
}

// ---- SCREENSHOT -----------------------------------------------------------------------------

void cmdScreenshot(std::string_view /*arg*/) {
  RenderLock lock;  // the render task must not draw into the frame while it is sent
  if (!screen->hasFrameBuffer()) {
    reply("SCREENSHOT_ERROR:framebuffer lent");
    return;
  }
  const size_t size = screen->getBufferSize();
  // The size comes first, as in release builds (scripts/debugging_monitor.py reads it there).
  char head[48];
  snprintf(head, sizeof(head), "SCREENSHOT_START:%u:%u:%u", static_cast<unsigned>(size),
           static_cast<unsigned>(screen->getDisplayWidth()), static_cast<unsigned>(screen->getDisplayHeight()));
  reply(head);
  // A frame cut short must not be followed by SCREENSHOT_END: the host would take it as complete.
  if (writeAll(screen->getFrameBuffer(), size, FRAME_STALL_MS)) {
    reply("SCREENSHOT_END");
  } else {
    lineLeftOpen = true;
    reply("SCREENSHOT_ERROR:transfer stalled");
  }
}

// ---- Dispatch -------------------------------------------------------------------------------

void cmdHelp(std::string_view arg);

using Handler = void (*)(std::string_view arg);
struct Command {
  const char* name;
  Handler run;
};
constexpr Command COMMANDS[] = {
    {"BTN", &cmdButton}, {"ACT", &cmdActivity},          {"STATE", &cmdState},
    {"IDLE", &cmdIdle},  {"SCREENSHOT", &cmdScreenshot}, {"HELP", &cmdHelp},
};

void cmdHelp(std::string_view /*arg*/) {
  ReplyLine line = startReply("HELP:");
  for (const Command& command : COMMANDS) {
    line.append(" ");
    line.append(command.name);
  }
  send(line);
}

void handleLine(const std::string_view line) {
  const std::string_view command = SerialCommandGrammar::trim(line.substr(4));  // after "CMD:"
  std::string_view name;
  std::string_view arg;
  SerialCommandGrammar::splitHead(command, name, arg);
  const auto* entry =
      std::find_if(std::begin(COMMANDS), std::end(COMMANDS), [&](const Command& c) { return name == c.name; });
  if (entry == std::end(COMMANDS)) {
    reply("CMD_ERROR:unknown command ", command);
    return;
  }
  entry->run(arg);
}

// ---- Input line -----------------------------------------------------------------------------

// A line is put together over as many passes as it takes to arrive: readStringUntil() would wait
// up to the 1 s stream timeout for the rest of it, and hold the whole loop up meanwhile. With its
// line end, a line must fit HWCDC's 256-byte receive buffer, or its end is lost before it is read.
char lineBuffer[255];
size_t lineLength = 0;
bool lineOverflowed = false;

bool isCommand(const std::string_view line) { return line.substr(0, 4) == "CMD:"; }

// ---- service() helpers ----------------------------------------------------------------------

void servicePresses() {
  if (presses.idle() && heldButtons == 0) return;
  uint8_t committed = 0;
  for (uint8_t i = 0; i <= HalGPIO::BTN_POWER; ++i) {
    if (gpio.isPressed(i)) committed |= static_cast<uint8_t>(1u << i);
  }
  heldButtons = presses.step(millis(), committed);
  gpio.holdSyntheticButtons(heldButtons);
}

const char* busyReason() {
  if (activityManager.hasPendingTransition()) return "transition";
  if (activityManager.isRenderBusy()) return "render";
  return nullptr;
}

void serviceIdle() {
  if (!idleGate.active()) return;
  IdleGate::Sample sample;
  sample.nowMs = millis();
  sample.busy = busyReason();
  sample.effectHandled = presses.idle();
  sample.renderPasses = activityManager.renderPassCount();
  sample.droppedPresses = presses.droppedPresses();
  sample.takenPresses = presses.takenPresses();
  switch (idleGate.step(sample)) {
    case IdleGate::Verdict::Waiting:
      break;
    case IdleGate::Verdict::Idle:
      replyNumber("IDLE:", idleGate.elapsedMs(sample.nowMs));
      break;
    case IdleGate::Verdict::TimedOut:
      reply("IDLE_TIMEOUT:", idleGate.reason());
      break;
    case IdleGate::Verdict::PressDropped:
      reply("IDLE_ERROR:press dropped");
      break;
  }
}

}  // namespace

bool poll(GfxRenderer& renderer) {
  screen = &renderer;
  while (logSerial.available() > 0) {
    const int c = logSerial.read();
    if (c < 0) break;
    if (c != '\n' && c != '\r') {
      if (lineLength < sizeof(lineBuffer)) {
        lineBuffer[lineLength++] = static_cast<char>(c);
      } else {
        lineOverflowed = true;
      }
      continue;
    }
    // A line end. CR LF ends one line; the empty line between the two is skipped.
    const std::string_view line(lineBuffer, lineLength);
    const bool overflowed = lineOverflowed;
    lineLength = 0;
    lineOverflowed = false;
    if (!isCommand(line)) continue;  // log noise, a terminal's stray keys
    // At the low-power clock the USB link flaps (see Logging.cpp) and HWCDC can drop what is
    // written meanwhile. The loop restores full speed for a command only after poll() returns.
    powerManager.setPowerSaving(false);
    if (overflowed) {
      reply("CMD_ERROR:line too long");
    } else {
      handleLine(line);
    }
    return true;  // one command per pass: the loop acts on it before the next is read
  }
  return false;
}

bool service() {
  const bool working = !presses.idle() || heldButtons != 0 || idleGate.isWaiting();
  servicePresses();
  serviceIdle();
  return working;
}

}  // namespace SerialTestApi

#endif
