#include "SerialConsole.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <string>

#include "CrossPointState.h"
#include "SilentRestart.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "util/SerialCommand.h"
#include "util/SerialLineBuffer.h"

namespace {

void replyOk(const SerialCommand& cmd, const char* detail = "") {
  logSerial.printf("CMD_OK %.*s%s%s\n", static_cast<int>(cmd.name.size()), cmd.name.data(), *detail ? " " : "", detail);
}

void replyErr(const SerialCommand& cmd, const char* reason) {
  // Also in the device log, worded so the host never mistakes it for the reply.
  // A busy click is routine (the host retries), so it stays at debug level.
  if (std::strcmp(reason, "busy") == 0) {
    LOG_DBG("CON", "%.*s refused: %s", static_cast<int>(cmd.name.size()), cmd.name.data(), reason);
  } else {
    LOG_ERR("CON", "%.*s refused: %s", static_cast<int>(cmd.name.size()), cmd.name.data(), reason);
  }
  logSerial.printf("CMD_ERR %.*s %s\n", static_cast<int>(cmd.name.size()), cmd.name.data(), reason);
}

void sendScreenshot() {
  // Like the button screenshot: keep the render task from drawing into the
  // frame while it streams, so a capture never mixes two frames.
  RenderLock lock;
  const uint32_t bufferSize = display.getBufferSize();
  const uint8_t* frame = display.getFrameBuffer();
  logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
  // write() can take only part of the frame when the USB TX buffer is full, so
  // keep sending the rest; give up only after a second without progress.
  constexpr unsigned long STALL_LIMIT_MS = 1000;
  uint32_t sent = 0;
  unsigned long lastProgress = millis();
  while (sent < bufferSize) {
    const size_t n = logSerial.write(frame + sent, bufferSize - sent);
    if (n > 0) {
      sent += n;
      lastProgress = millis();
    } else if (millis() - lastProgress >= STALL_LIMIT_MS) {
      break;
    } else {
      delay(1);
    }
  }
  logSerial.printf("SCREENSHOT_END\n");
  if (sent < bufferSize) LOG_ERR("CON", "Screenshot stalled after %u of %u bytes", sent, bufferSize);
}

#ifdef ENABLE_SERIAL_CONTROL
bool homePending = false;
unsigned long homeRequestedAt = 0;
constexpr unsigned long HOME_REPLY_TIMEOUT_MS = 10000;

void finishHome() {
  if (!homePending) return;
  constexpr SerialCommand home = parseSerialCommand("HOME");
  if (activityManager.isForegroundHomeShown()) {
    homePending = false;
    replyOk(home);
  } else if (millis() - homeRequestedAt >= HOME_REPLY_TIMEOUT_MS) {
    homePending = false;
    replyErr(home, "Home did not appear");
  }
}

void sendState(const SerialCommand& cmd) {
  const auto heap = HalMemory::getInternalHeap();
  char detail[384];
  // The book path goes last: it may contain spaces.
  snprintf(detail, sizeof(detail), "activity=%s reader=%d uptime=%lu free=%u largest=%u minfree=%u book=%s",
           activityManager.foregroundName(), activityManager.isReaderActivity() ? 1 : 0, millis(),
           static_cast<unsigned>(heap.freeBytes), static_cast<unsigned>(heap.largestBlockBytes),
           static_cast<unsigned>(heap.minFreeBytes), APP_STATE.openEpubPath.c_str());
  replyOk(cmd, detail);
}

void openBook(const SerialCommand& cmd) {
  const std::string path(cmd.path);
  if (!Storage.exists(path.c_str())) return replyErr(cmd, "no such file");
  if (cmd.cold) {
    if (!FsHelpers::hasEpubExtension(path)) return replyErr(cmd, "COLD supports EPUB only");
    // The reader keeps its chapter file open; drop the layouts only once it has closed.
    if (activityManager.isReaderActivity()) return replyErr(cmd, "close the book first (HOME)");
    const std::string sections = Epub(path, "/.crosspoint").getCachePath() + "/sections";
    if (Storage.exists(sections.c_str()) && !Storage.removeDir(sections.c_str())) {
      return replyErr(cmd, "could not remove the laid-out chapters");
    }
  }
  if (!activityManager.goToReader(path)) return replyErr(cmd, "the reader could not open it");
  replyOk(cmd);
}

void restart(const SerialCommand& cmd) {
  if (cmd.restart == SerialCommand::RestartTarget::Reader && APP_STATE.openEpubPath.empty()) {
    return replyErr(cmd, "no open book");
  }
  replyOk(cmd);
  logSerial.flush();
  delay(50);
  switch (cmd.restart) {
    case SerialCommand::RestartTarget::Boot:
      ESP.restart();
      break;
    case SerialCommand::RestartTarget::Home:
      silentRestart();
      break;
    case SerialCommand::RestartTarget::Reader:
      silentRestartToReader();
      break;
  }
}
#endif  // ENABLE_SERIAL_CONTROL

void runCommand(const SerialCommand& cmd) {
#ifndef ENABLE_SERIAL_CONTROL
  // Release builds keep only the screenshot, as before this console existed.
  if (cmd.type == SerialCommand::Type::Screenshot) return sendScreenshot();
  return replyErr(cmd, "serial control is not in this build");
#else
  switch (cmd.type) {
    case SerialCommand::Type::Invalid:
      return replyErr(cmd, cmd.error);
    case SerialCommand::Type::Ping: {
      char detail[32];
      snprintf(detail, sizeof(detail), "uptime=%lu", millis());
      return replyOk(cmd, detail);
    }
    case SerialCommand::Type::State:
      return sendState(cmd);
    case SerialCommand::Type::Screenshot:
      return sendScreenshot();
    case SerialCommand::Type::Click:
      // Busy while a frame is pending or drawing; the host retries.
      return activityManager.consoleClick(cmd.click) ? replyOk(cmd) : replyErr(cmd, "busy");
    case SerialCommand::Type::Turn:
      // Page turns only; menus take CLICK NEXT/PREV.
      if (!activityManager.isForegroundReader()) return replyErr(cmd, "no book in front");
      return activityManager.remoteTurn(cmd.forward, cmd.chapter) ? replyOk(cmd) : replyErr(cmd, "busy");
    case SerialCommand::Type::Menu:
      return activityManager.remoteShortcut(true) ? replyOk(cmd) : replyErr(cmd, "busy or no book in front");
    case SerialCommand::Type::Home:
      // Answered once Home has painted (pollSerialConsole), so a script's next
      // step never races the reader closing.
      activityManager.goHome();
      homeRequestedAt = millis();
      homePending = true;
      return;
    case SerialCommand::Type::Open:
      return openBook(cmd);
    case SerialCommand::Type::Restart:
      return restart(cmd);
  }
#endif
}

}  // namespace

void pollSerialConsole() {
#ifdef ENABLE_SERIAL_CONTROL
  finishHome();
#endif
  // Act only on a full line, so a slow or partial write never stalls the main
  // loop or runs a cut-off command.
#ifdef ENABLE_SERIAL_CONTROL
  static SerialLineBuffer<256> input;  // room for OPEN with a long path
#else
  static SerialLineBuffer<32> input;  // CMD:SCREENSHOT is all a release build takes
#endif
  while (logSerial.available() > 0) {
    const int byte = logSerial.read();
    if (byte < 0 || !input.feed(static_cast<char>(byte))) continue;
    const std::string_view text = input.line();
    if (text.substr(0, 4) != "CMD:") continue;
    const SerialCommand cmd = parseSerialCommand(text.substr(4));
    if (input.overflowed()) {
      replyErr(cmd, "line too long");
      continue;
    }
    runCommand(cmd);
    return;  // one command per main-loop pass, so an injected click reaches its frame
  }
}
