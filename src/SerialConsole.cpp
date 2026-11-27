#include "SerialConsole.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalMemory.h>
#include <HalSerialBulkRead.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdarg>
#include <cstring>
#include <string>

#include "CrossPointState.h"
#include "SerialFileCommands.h"
#include "SilentRestart.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "util/SerialCommand.h"
#include "util/SerialLineBuffer.h"
#include "util/SerialTransfer.h"

namespace serialconsole {

void sendLine(const char* format, ...) {
  // HWCDC gives up on its TX lock after one tick and writes nothing; a partial
  // write is not retried, so a line is never sent twice.
  for (int attempt = 0; attempt < 5; attempt++) {
    va_list args;
    va_start(args, format);
    const size_t written = logSerial.vprintf(format, args);  // vprintf ends args itself
    if (written > 0) return;
  }
}

void replyOk(const SerialCommand& cmd, const char* detail) {
  sendLine("CMD_OK %.*s%s%s\n", static_cast<int>(cmd.name.size()), cmd.name.data(), *detail ? " " : "", detail);
}

void replyErr(const SerialCommand& cmd, const char* reason) {
  // Also in the device log, worded so the host never mistakes it for the reply.
  // A busy click is routine (the host retries), so it stays at debug level.
  if (std::strcmp(reason, "busy") == 0) {
    LOG_DBG("CON", "%.*s refused: %s", static_cast<int>(cmd.name.size()), cmd.name.data(), reason);
  } else {
    LOG_ERR("CON", "%.*s refused: %s", static_cast<int>(cmd.name.size()), cmd.name.data(), reason);
  }
  sendLine("CMD_ERR %.*s %s\n", static_cast<int>(cmd.name.size()), cmd.name.data(), reason);
}

}  // namespace serialconsole

namespace {

using serialconsole::replyErr;
using serialconsole::replyOk;

bool isFileCommand(const SerialCommand& cmd) {
  switch (cmd.type) {
    case SerialCommand::Type::List:
    case SerialCommand::Type::Get:
    case SerialCommand::Type::Put:
    case SerialCommand::Type::MakeDir:
    case SerialCommand::Type::Remove:
    case SerialCommand::Type::Move:
      return true;
    default:
      return false;
  }
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

// Whether this build carries the command out; false after refusing it.
bool runCommand(const SerialCommand& cmd) {
  if (cmd.type == SerialCommand::Type::Screenshot) {
    sendScreenshot();
    return true;
  }
  if (isFileCommand(cmd)) {
#ifdef ENABLE_SERIAL_FILES
    runSerialFileCommand(cmd);
    return true;
#else
    replyErr(cmd, "file commands are not in this build");
    return false;
#endif
  }
#ifndef ENABLE_SERIAL_CONTROL
  // Release builds keep the screenshot and the file commands only.
  replyErr(cmd, cmd.type == SerialCommand::Type::Invalid ? cmd.error : "serial control is not in this build");
  return false;
#else
  switch (cmd.type) {
    case SerialCommand::Type::Invalid:
      replyErr(cmd, cmd.error);
      break;
    case SerialCommand::Type::Ping: {
      char detail[32];
      snprintf(detail, sizeof(detail), "uptime=%lu", millis());
      replyOk(cmd, detail);
      break;
    }
    case SerialCommand::Type::State:
      sendState(cmd);
      break;
    case SerialCommand::Type::Click:
      // Busy while a frame is pending or drawing; the host retries.
      activityManager.consoleClick(cmd.click) ? replyOk(cmd) : replyErr(cmd, "busy");
      break;
    case SerialCommand::Type::Turn:
      // Page turns only; menus take CLICK NEXT/PREV.
      if (!activityManager.isForegroundReader()) {
        replyErr(cmd, "no book in front");
      } else {
        activityManager.remoteTurn(cmd.forward, cmd.chapter) ? replyOk(cmd) : replyErr(cmd, "busy");
      }
      break;
    case SerialCommand::Type::Menu:
      activityManager.remoteShortcut(true) ? replyOk(cmd) : replyErr(cmd, "busy or no book in front");
      break;
    case SerialCommand::Type::Home:
      // Answered once Home has painted (pollSerialConsole), so a script's next
      // step never races the reader closing.
      activityManager.goHome();
      homeRequestedAt = millis();
      homePending = true;
      break;
    case SerialCommand::Type::Open:
      openBook(cmd);
      break;
    case SerialCommand::Type::Restart:
      restart(cmd);
      break;
    default:
      break;
  }
  return true;
#endif
}

}  // namespace

bool pollSerialConsole() {
  HalSerialBulkRead::restoreReceiving();
#ifdef ENABLE_SERIAL_CONTROL
  finishHome();
#endif
  // Act only on a full line, so a slow or partial write never stalls the main
  // loop or runs a cut-off command.
#if defined(ENABLE_SERIAL_CONTROL) || defined(ENABLE_SERIAL_FILES)
  static SerialLineBuffer<serialxfer::MAX_LINE_LENGTH> input;
#else
  static SerialLineBuffer<32> input;  // CMD:SCREENSHOT is all such a build takes
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
    // One command per main-loop pass, so an injected click reaches its frame.
    return runCommand(cmd);
  }
  return false;
}
