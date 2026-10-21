#include "SerialConsole.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>

#include <string>

#include "CrossPointState.h"
#include "SilentRestart.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "util/SerialCommand.h"

namespace {

void replyOk(const SerialCommand& cmd, const char* detail = "") {
  logSerial.printf("CMD_OK %.*s%s%s\n", static_cast<int>(cmd.name.size()), cmd.name.data(), *detail ? " " : "", detail);
}

void replyErr(const SerialCommand& cmd, const char* reason) {
  logSerial.printf("CMD_ERR %.*s %s\n", static_cast<int>(cmd.name.size()), cmd.name.data(), reason);
}

void sendScreenshot() {
  const uint32_t bufferSize = display.getBufferSize();
  logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
  logSerial.write(display.getFrameBuffer(), bufferSize);
  logSerial.printf("SCREENSHOT_END\n");
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
  activityManager.goToReader(path);
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

}  // namespace

void pollSerialConsole() {
  if (logSerial.available() <= 0) return;
  const String line = logSerial.readStringUntil('\n');
  if (!line.startsWith("CMD:")) return;
  const SerialCommand cmd = parseSerialCommand(std::string_view(line.c_str() + 4, line.length() - 4));

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
      return activityManager.remoteTurn(cmd.forward, cmd.chapter) ? replyOk(cmd)
                                                                  : replyErr(cmd, "busy or no book in front");
    case SerialCommand::Type::Menu:
      return activityManager.remoteShortcut(true) ? replyOk(cmd) : replyErr(cmd, "busy or no book in front");
    case SerialCommand::Type::Home:
      activityManager.goHome();
      return replyOk(cmd);
    case SerialCommand::Type::Open:
      return openBook(cmd);
    case SerialCommand::Type::Restart:
      return restart(cmd);
  }
}
