#include "SerialTestApi.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "BootDiagnostics.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "SettingsByKey.h"
#include "SettingsList.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/home/LibraryTab.h"
#include "util/Dictionary.h"
#include "util/ScreenshotUtil.h"
#include "util/SerialCommandGrammar.h"

namespace SerialTestApi {

namespace {

using SerialCommandGrammar::appendField;
using SerialCommandGrammar::appendValue;

GfxRenderer* testRenderer = nullptr;

// ---- Output ---------------------------------------------------------------------------------

// logSerial.write() returns short when the USB-CDC TX ring stays full past its timeout (1 ms
// outside a transfer), so every line is written in a loop until the host has taken it. Yields
// rather than spins: a tight loop on the C3 starves the USB-CDC task being waited on. Gives up
// after `stallMs` without progress (the host stopped reading).
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

// A reply waits up to 2 s for the host; an event line (sent unasked once EVENTS:ON) only 50 ms,
// so a host that enabled events and went away cannot hold the loop up.
constexpr unsigned long kReplyStallMs = 2000;
constexpr unsigned long kEventStallMs = 50;

void writeLine(const std::string_view text, const unsigned long stallMs = kReplyStallMs) {
  if (writeAll(reinterpret_cast<const uint8_t*>(text.data()), text.size(), stallMs)) {
    const uint8_t newline = '\n';
    writeAll(&newline, 1, stallMs);
  }
}

// A reply short enough for a stack buffer (anything with a path or a label goes through a
// std::string and writeLine() instead).
void replyf(const char* format, ...) {
  char line[160];
  va_list args;
  va_start(args, format);
  const int n = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (n > 0) writeLine(std::string_view(line, std::min(static_cast<size_t>(n), sizeof(line) - 1)));
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}

// "NAME" and "rest" of "NAME:rest".
void splitHead(const std::string_view text, std::string_view& head, std::string_view& rest) {
  const size_t colon = text.find(':');
  head = text.substr(0, colon);
  rest = colon == std::string_view::npos ? std::string_view() : text.substr(colon + 1);
}

// ---- Probes ---------------------------------------------------------------------------------

// Filled from whichever task draws (render task, loop task), read on the loop task. The critical
// section covers a copy of a few dozen bytes. `activity` is ActivityManager::activityChangeCount()
// when noted: what is noted on one screen is never reported for the next. `renderPass` is
// ActivityManager::renderPassCount(): a popup is on screen until the next pass redraws it.
portMUX_TYPE probeMux = portMUX_INITIALIZER_UNLOCKED;
constexpr size_t kProbeText = 64;  // a popup message or a row label; longer ones are cut

struct TextProbe {
  char text[kProbeText] = {};
  int index = -1;
  uint32_t activity = 0;
  uint32_t renderPass = 0;
  uint32_t atMs = 0;
  uint32_t sequence = 0;  // counts notes, so a host-side change detector sees a repeat too
  bool set = false;
};
TextProbe popupProbe;
TextProbe selectionProbe;

void noteText(TextProbe& probe, const int index, const char* text) {
  const uint32_t activity = activityManager.activityChangeCount();
  const uint32_t renderPass = activityManager.renderPassCount();
  const uint32_t now = millis();
  portENTER_CRITICAL(&probeMux);
  size_t i = 0;
  // One line on the wire: control characters (a popup's line breaks) become spaces.
  for (; text && text[i] != '\0' && i + 1 < kProbeText; ++i) {
    probe.text[i] = static_cast<unsigned char>(text[i]) < 0x20 ? ' ' : text[i];
  }
  // A text cut short must not end in half a UTF-8 character: drop its continuation bytes and lead.
  if (text && text[i] != '\0') {
    while (i > 0 && (static_cast<unsigned char>(probe.text[i - 1]) & 0xC0) == 0x80) --i;
    if (i > 0 && static_cast<unsigned char>(probe.text[i - 1]) >= 0xC0) --i;
  }
  probe.text[i] = '\0';
  probe.index = index;
  probe.activity = activity;
  probe.renderPass = renderPass;
  probe.atMs = now;
  probe.sequence++;
  probe.set = true;
  portEXIT_CRITICAL(&probeMux);
}

TextProbe readProbe(const TextProbe& probe) {
  portENTER_CRITICAL(&probeMux);
  const TextProbe copy = probe;
  portEXIT_CRITICAL(&probeMux);
  return copy;
}

// Section builds finished since service() last looked. Four, as with the touch ring: a backlog
// deeper than that means nobody is reading the events anyway.
struct BuildDone {
  int16_t spine;
  uint16_t pages;
};
constexpr int kBuildRing = 4;
BuildDone buildRing[kBuildRing] = {};
uint8_t buildHead = 0;
uint8_t buildTail = 0;

// ---- State shared by the commands and service() ---------------------------------------------

bool eventsOn = false;
uint32_t reportedActivity = 0;
uint32_t reportedPopup = 0;

struct IdleWait {
  bool active = false;
  bool screenOnly = false;
  uint32_t startMs = 0;
  uint32_t timeoutMs = 0;
  uint32_t quietSinceMs = 0;
  bool quiet = false;
  const char* lastBusy = "settling";  // nothing was busy, but not for kIdleQuietMs yet
};
IdleWait idleWait;
// How long nothing may be busy before CMD:IDLE answers: longer than the gaps between the steps of
// one piece of work (a press handled on the next pass, its render queued at the end of that pass,
// a reader build slice per pass).
constexpr uint32_t kIdleQuietMs = 250;
constexpr uint32_t kIdleDefaultTimeoutMs = 10000;
constexpr uint32_t kIdleMaxTimeoutMs = 600000;

PowerRequest powerRequest = PowerRequest::None;

// ---- BTN ------------------------------------------------------------------------------------

void cmdButton(const std::string_view arg) {
  using SerialCommandGrammar::PressKind;
  std::string_view name;
  PressKind kind = PressKind::Short;
  if (!SerialCommandGrammar::parseButtonArg(arg, name, kind)) {
    writeLine(std::string("BTN_ERROR:bad press ") + std::string(arg));
    return;
  }
  struct NamedButton {
    const char* name;
    uint8_t index;
  };
  static constexpr NamedButton kButtons[] = {{"BACK", HalGPIO::BTN_BACK},  {"CONFIRM", HalGPIO::BTN_CONFIRM},
                                             {"LEFT", HalGPIO::BTN_LEFT},  {"RIGHT", HalGPIO::BTN_RIGHT},
                                             {"UP", HalGPIO::BTN_UP},      {"DOWN", HalGPIO::BTN_DOWN},
                                             {"POWER", HalGPIO::BTN_POWER}};
  for (const NamedButton& button : kButtons) {
    if (name != button.name) continue;
    switch (kind) {
      case PressKind::Short:
        gpio.injectPress(button.index);
        break;
      case PressKind::Long:
        if (button.index == HalGPIO::BTN_POWER) {
          // Holding power sleeps the device, and that is main.cpp's hold timer reading the live
          // pin, which an injected press never reaches. Do what the hold does.
          powerRequest = PowerRequest::Sleep;
          break;
        }
        gpio.injectPress(button.index, /*longPress=*/true);
        break;
      case PressKind::Double: {
        // Two taps inside ButtonEventManager::DOUBLE_WINDOW_MS (300 ms), dated apart so the
        // classifier sees two presses. A button with no double action configured takes them as two
        // short presses, as it would from a finger.
        constexpr uint32_t kTapGapMs = 120;
        const uint32_t now = millis();
        gpio.injectPress(button.index, false, now > kTapGapMs ? now - kTapGapMs : 0);
        gpio.injectPress(button.index, false, now);
        break;
      }
    }
    writeLine("BTN_OK");
    return;
  }
  writeLine(std::string("BTN_ERROR:unknown button ") + std::string(name));
}

// ---- ACT ------------------------------------------------------------------------------------

bool openReader(const std::string_view arg) {
  SerialCommandGrammar::ReaderTarget target;
  if (!SerialCommandGrammar::parseReaderTarget(arg, target)) {
    writeLine(std::string("ACT_ERROR:bad position ") + std::string(arg));
    return false;
  }
  std::string path(target.path);
  if (!Storage.exists(path.c_str())) {
    writeLine("ACT_ERROR:no such file " + path);
    return false;
  }
  using Kind = SerialCommandGrammar::ReaderTarget::Kind;
  switch (target.kind) {
    case Kind::Saved:
      break;
    case Kind::SpinePage: {
      // The hand-off the bookmark list uses (0-based); the reader applies it in onEnter().
      auto& jump = APP_STATE.pendingBookmarkJump;
      jump.active = true;
      jump.bookPath = path;
      jump.spineIndex = static_cast<uint16_t>(target.spine - 1);
      jump.pageNumber = static_cast<uint16_t>(target.page - 1);
      APP_STATE.saveToFile();
      break;
    }
    case Kind::Percent: {
      auto& jump = APP_STATE.pendingPercentJump;
      jump.active = true;
      jump.bookPath = path;
      jump.percent = static_cast<uint8_t>(target.percent);
      break;
    }
  }
  activityManager.goToReader(std::move(path));
  return true;
}

// SettingsActivity's tabs, in its categoryNames order.
bool settingsTab(const std::string_view name, int& tab) {
  static constexpr const char* kTabs[] = {"DISPLAY", "READER", "CONTROLS", "SYSTEM"};
  for (int i = 0; i < static_cast<int>(sizeof(kTabs) / sizeof(kTabs[0])); ++i) {
    if (name == kTabs[i]) {
      tab = i;
      return true;
    }
  }
  return SerialCommandGrammar::parseInt(name, tab) && tab >= 0 && tab < 4;
}

bool libraryTab(const std::string_view name, LibraryTab& tab) {
  struct NamedTab {
    const char* name;
    LibraryTab tab;
  };
  static constexpr NamedTab kTabs[] = {{"BOOKS", LibraryTab::Books},
                                       {"RECENT", LibraryTab::Recent},
                                       {"NEW", LibraryTab::New},
                                       {"AUTHORS", LibraryTab::Authors}};
  for (const NamedTab& named : kTabs) {
    if (name == named.name) {
      tab = named.tab;
      return true;
    }
  }
  return false;
}

// CMD:ACT:DICT:<word>. The dictionary screen's lookup without the screen: open the configured
// dictionary, build its index if it needs one, look the word up, and say what came of it.
void lookUpWord(const std::string_view wordView) {
  if (wordView.empty()) {
    writeLine("DICT_ERROR:no word");
    return;
  }
  const std::string text(wordView);  // not `word`: Arduino.h typedefs that
  const char* dictionaryName = SETTINGS.dictionaryName;
  std::string reply = "DICT:result=";
  const char* outcome = nullptr;
  std::string definition;
  std::string headword;
  bool found = false;
  if (dictionaryName[0] == '\0') {
    outcome = "no_dictionary";
  } else {
    // The object carries scan buffers and an inflate window between lookups; on the heap for the
    // length of this command only, and makeUniqueNoThrow so a fragmented heap is an answer, not
    // an abort.
    auto dict = makeUniqueNoThrow<Dictionary>();
    if (!dict) {
      outcome = "low_memory";
    } else if (!dict->open(dictionaryName)) {
      outcome = "open_failed";
    } else if (dict->needsIndex() && !dict->buildIndex([](void*) { vTaskDelay(1); })) {
      outcome = "index_failed";
    } else {
      Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
      found = dict->lookup(text.c_str(), definition, headword, &result);
      outcome = Dictionary::resultName(result);
    }
  }
  Dictionary::logLookup(dictionaryName, text.c_str(), outcome, found ? headword.c_str() : nullptr);
  reply += outcome;
  appendField(reply, "dict", dictionaryName);
  appendField(reply, "word", text);
  if (found) {
    appendField(reply, "headword", headword);
    appendField(reply, "bytes", static_cast<long>(definition.size()));
  }
  writeLine(reply);
}

void cmdActivity(const std::string_view arg) {
  std::string_view target;
  std::string_view rest;
  splitHead(arg, target, rest);
  if (target == "HOME") {
    activityManager.goHome();
  } else if (target == "READER") {
    if (!openReader(rest)) return;
  } else if (target == "SETTINGS") {
    int tab = 0;
    if (!rest.empty() && !settingsTab(rest, tab)) {
      writeLine(std::string("ACT_ERROR:unknown settings tab ") + std::string(rest));
      return;
    }
    activityManager.goToSettings(tab);
  } else if (target == "BOOKMARKS") {
    activityManager.goToGlobalBookmarks();
  } else if (target == "FILES") {
    activityManager.goToLibrary(LibraryTab::Books, std::string(rest));
  } else if (target == "LIBRARY") {
    LibraryTab tab = LibraryTab::Books;
    if (!rest.empty() && !libraryTab(rest, tab)) {
      writeLine(std::string("ACT_ERROR:unknown library tab ") + std::string(rest));
      return;
    }
    activityManager.goToLibrary(tab);
  } else if (target == "FILE_TRANSFER") {
    activityManager.goToFileTransfer();
  } else if (target == "USB_TRANSFER") {
    // Leaving the transfer screen reboots (SerialTransferActivity::onExit), so replacing it with
    // a fresh one would land on Home: already there is already done.
    if (std::strcmp(activityManager.currentActivityName(), "SerialTransfer") != 0) {
      activityManager.goToSerialTransfer();
    }
  } else if (target == "DICT") {
    lookUpWord(rest);
    return;
  } else {
    writeLine(std::string("ACT_ERROR:unknown activity ") + std::string(target));
    return;
  }
  writeLine("ACT_OK");
}

// ---- STATE ----------------------------------------------------------------------------------

void cmdState(std::string_view /*arg*/) {
  std::string state = "STATE:";
  state.reserve(320);
  {
    RenderLock lock;  // the activity's state is also read by the render task
    activityManager.describeState(state);
  }
  const uint32_t activity = activityManager.activityChangeCount();
  const TextProbe selection = readProbe(selectionProbe);
  // Screens that know their own selection (Home) wrote sel= already.
  if (selection.set && selection.activity == activity && state.find(" sel=") == std::string::npos) {
    appendField(state, "sel", static_cast<long>(selection.index));
    if (selection.text[0] != '\0') appendField(state, "label", selection.text);
  }
  const TextProbe popup = readProbe(popupProbe);
  if (popup.set && popup.activity == activity && popup.renderPass == activityManager.renderPassCount()) {
    appendField(state, "popup", popup.text);
    appendField(state, "popup_ms", static_cast<long>(millis() - popup.atMs));
  }
  appendField(state, "heap", static_cast<long>(ESP.getFreeHeap()));
  appendField(state, "maxblock",
              static_cast<long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  writeLine(state);
}

// ---- IDLE / EVENTS --------------------------------------------------------------------------

void cmdIdle(const std::string_view arg) {
  // [<timeout_ms>][:SCREEN]
  std::string_view first;
  std::string_view second;
  splitHead(arg, first, second);
  int timeout = static_cast<int>(kIdleDefaultTimeoutMs);
  bool screenOnly = false;
  if (first == "SCREEN") {
    screenOnly = true;
  } else if (!first.empty() && (!SerialCommandGrammar::parseInt(first, timeout) || timeout <= 0)) {
    writeLine(std::string("IDLE_ERROR:bad timeout ") + std::string(first));
    return;
  }
  if (second == "SCREEN") {
    screenOnly = true;
  } else if (!second.empty()) {
    writeLine(std::string("IDLE_ERROR:bad mode ") + std::string(second));
    return;
  }
  idleWait = IdleWait{};
  idleWait.active = true;
  idleWait.screenOnly = screenOnly;
  idleWait.startMs = millis();
  idleWait.timeoutMs = std::min(static_cast<uint32_t>(timeout), kIdleMaxTimeoutMs);
}

void cmdEvents(const std::string_view arg) {
  if (arg == "ON") {
    eventsOn = true;
  } else if (arg == "OFF") {
    eventsOn = false;
  } else if (!arg.empty()) {
    writeLine(std::string("EVENTS_ERROR:expected ON or OFF, got ") + std::string(arg));
    return;
  }
  replyf("EVENTS_OK:%s", eventsOn ? "on" : "off");
}

// ---- GET / SET ------------------------------------------------------------------------------

// Hands the settings row with this key to `use`; false when there is none.
template <typename Use>
bool withSetting(const std::string_view key, Use&& use) {
  bool found = false;
  forEachSetting([&](const SettingInfo& s) {
    if (found || !s.key || key != s.key) return;
    found = true;
    use(s);
  });
  return found;
}

// "<key>=<value>[ label=<option label>]" for a GET or SET_OK reply.
void appendSetting(std::string& line, const SettingInfo& s) {
  line += s.key;
  line += '=';
  int number = 0;
  if (SettingsByKey::readNumber(s, number)) {
    line += std::to_string(number);
    if (s.type == SettingType::ENUM && number >= 0 && number < SettingsByKey::optionCount(s)) {
      appendField(line, "label", SettingsByKey::optionLabel(s, static_cast<uint8_t>(number)));
    }
    return;
  }
  if (s.type != SettingType::STRING) {
    line += "(none)";
    return;
  }
  // Credentials never leave the device, as on the web settings page.
  if (s.obfuscated) {
    line += "(hidden)";
    return;
  }
  if (s.stringGetter) {
    appendValue(line, s.callStringGetter());
  } else if (s.stringMaxLen > 0) {
    appendValue(line, reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset);
  } else {
    appendValue(line, "");
  }
}

void cmdGet(const std::string_view key) {
  if (key == "*") {
    int count = 0;
    forEachSetting([&](const SettingInfo& s) {
      if (!s.key) return;
      std::string line = "GET:";
      appendSetting(line, s);
      writeLine(line);
      count++;
    });
    replyf("GET_END:%d", count);
    return;
  }
  std::string line = "GET:";
  if (!withSetting(key, [&](const SettingInfo& s) { appendSetting(line, s); })) {
    writeLine(std::string("GET_ERROR:unknown key ") + std::string(key));
    return;
  }
  writeLine(line);
}

void cmdSet(const std::string_view arg) {
  std::string_view key;
  std::string_view value;
  if (!SerialCommandGrammar::splitKeyValue(arg, key, value)) {
    writeLine("SET_ERROR:expected <key>=<value>");
    return;
  }
  std::string reply;
  const bool known = withSetting(key, [&](const SettingInfo& s) {
    SettingsByKey::WriteResult result = SettingsByKey::WriteResult::NotWritable;
    if (s.type == SettingType::STRING) {
      result = SettingsByKey::writeString(s, std::string(value));
    } else {
      int number = 0;
      bool parsed = SerialCommandGrammar::parseInt(value, number);
      // An ENUM row also takes one of its option labels, as GET shows them.
      for (uint8_t i = 0; !parsed && s.type == SettingType::ENUM && i < SettingsByKey::optionCount(s); ++i) {
        if (value == SettingsByKey::optionLabel(s, i)) {
          number = i;
          parsed = true;
        }
      }
      if (!parsed) {
        reply = "SET_ERROR:not a number or option " + std::string(value);
        return;
      }
      result = SettingsByKey::writeNumber(s, number);
    }
    switch (result) {
      case SettingsByKey::WriteResult::Applied:
        reply = "SET_OK:";
        break;
      case SettingsByKey::WriteResult::OutOfRange:
        reply = "SET_ERROR:out of range " + std::string(value);
        return;
      case SettingsByKey::WriteResult::NotWritable:
        reply = "SET_ERROR:not writable " + std::string(key);
        return;
    }
  });
  if (!known) {
    writeLine(std::string("SET_ERROR:unknown key ") + std::string(key));
    return;
  }
  if (reply != "SET_OK:") {
    writeLine(reply);
    return;
  }
  // As the web settings page does: settle the settings that depend on others, save, and read the
  // value back so the reply shows what was stored (a dependent setting may have moved it).
  SettingsByKey::commit();
  withSetting(key, [&](const SettingInfo& s) { appendSetting(reply, s); });
  writeLine(reply);
}

// ---- LOG ------------------------------------------------------------------------------------

constexpr const char* kLevelTags[] = {"ERR", "INF", "DBG", "TRC"};

void cmdLog(const std::string_view arg) {
  uint8_t level = 0;
  if (!arg.empty()) {
    if (!SerialCommandGrammar::parseLogLevel(arg, level)) {
      writeLine(std::string("LOG_ERROR:unknown level ") + std::string(arg));
      return;
    }
    setLogWireLevel(level);
  }
  constexpr int kCompiled = LOG_LEVEL < 3 ? LOG_LEVEL : 3;
  replyf("LOG_OK:level=%s compiled=%s", kLevelTags[logWireLevel() < 3 ? logWireLevel() : 3], kLevelTags[kCompiled]);
}

// ---- TOUCH / SWIPE --------------------------------------------------------------------------

// Logical (x, y) on the screen as drawn now -> the touch HAL's normalized panel coordinates.
bool toTouch(const int x, const int y, float& nx, float& ny) {
  if (!testRenderer) return false;
  if (x < 0 || y < 0 || x >= testRenderer->getScreenWidth() || y >= testRenderer->getScreenHeight()) return false;
  testRenderer->logicalToTap(x, y, nx, ny);
  return true;
}

void cmdTouch(const std::string_view arg) {
  if (!gpio.hasTouch()) {
    writeLine("TOUCH_ERROR:no touch panel");
    return;
  }
  int xy[2];
  float nx = 0.0f;
  float ny = 0.0f;
  if (!SerialCommandGrammar::parseIntList(arg, xy, 2) || !toTouch(xy[0], xy[1], nx, ny)) {
    writeLine(std::string("TOUCH_ERROR:expected x,y on screen, got ") + std::string(arg));
    return;
  }
  gpio.injectTouch(nx, ny);
  writeLine("TOUCH_OK");
}

void cmdSwipe(const std::string_view arg) {
  if (!gpio.hasTouch()) {
    writeLine("TOUCH_ERROR:no touch panel");
    return;
  }
  int points[4];
  float nx = 0.0f;
  float ny = 0.0f;
  float nxEnd = 0.0f;
  float nyEnd = 0.0f;
  if (!SerialCommandGrammar::parseIntList(arg, points, 4) || !toTouch(points[0], points[1], nx, ny) ||
      !toTouch(points[2], points[3], nxEnd, nyEnd)) {
    writeLine(std::string("TOUCH_ERROR:expected x1,y1,x2,y2 on screen, got ") + std::string(arg));
    return;
  }
  gpio.injectSwipe(nx, ny, nxEnd, nyEnd);
  writeLine("SWIPE_OK");
}

// ---- Power and the rest ---------------------------------------------------------------------

void cmdSleep(std::string_view /*arg*/) {
  writeLine("SLEEP_OK");
  powerRequest = PowerRequest::Sleep;
}

void cmdReboot(std::string_view /*arg*/) {
  writeLine("REBOOT_OK");
  powerRequest = PowerRequest::Reboot;
}

void cmdScreenshot(std::string_view /*arg*/) { ScreenshotUtil::request(ScreenshotUtil::Target::Host); }

// On-demand replay of this session's boot summary, for when the monitor was attached too late to
// catch it and the automatic repeats are used up. The summary is log lines; the reply after them
// tells a script it is complete.
void cmdBootLog(std::string_view /*arg*/) {
  BootDiag::logSummary();
  writeLine("BOOTLOG_OK");
}

void cmdHelp(std::string_view arg);

using Handler = void (*)(std::string_view arg);
struct Command {
  const char* name;
  Handler run;
};
constexpr Command kCommands[] = {
    {"BTN", &cmdButton},    {"ACT", &cmdActivity},    {"STATE", &cmdState}, {"SCREENSHOT", &cmdScreenshot},
    {"IDLE", &cmdIdle},     {"EVENTS", &cmdEvents},   {"GET", &cmdGet},     {"SET", &cmdSet},
    {"LOG", &cmdLog},       {"TOUCH", &cmdTouch},     {"SWIPE", &cmdSwipe}, {"SLEEP", &cmdSleep},
    {"REBOOT", &cmdReboot}, {"BOOTLOG", &cmdBootLog}, {"HELP", &cmdHelp},
};

void cmdHelp(std::string_view /*arg*/) {
  std::string line = "HELP:";
  for (const Command& command : kCommands) {
    if (line.size() > 5) line += ' ';
    line += command.name;
  }
  writeLine(line);
}

// ---- service() helpers ----------------------------------------------------------------------

void serviceEvents() {
  const uint32_t activity = activityManager.activityChangeCount();
  const TextProbe popup = readProbe(popupProbe);
  BuildDone builds[kBuildRing];
  int buildCount = 0;
  portENTER_CRITICAL(&probeMux);
  while (buildHead != buildTail) {
    builds[buildCount++] = buildRing[buildHead];
    buildHead = static_cast<uint8_t>((buildHead + 1) % kBuildRing);
  }
  portEXIT_CRITICAL(&probeMux);
  // Nothing is written, and nothing piles up, while no host asked for events.
  if (!eventsOn) {
    reportedActivity = activity;
    reportedPopup = popup.sequence;
    return;
  }
  if (activity != reportedActivity) {
    reportedActivity = activity;
    writeLine(std::string("EVT:ACT ") + activityManager.currentActivityName(), kEventStallMs);
  }
  if (popup.sequence != reportedPopup) {
    reportedPopup = popup.sequence;
    writeLine(std::string("EVT:POPUP ") + popup.text, kEventStallMs);
  }
  for (int i = 0; i < buildCount; ++i) {
    char line[48];
    snprintf(line, sizeof(line), "EVT:BUILD_DONE spine=%d pages=%u", builds[i].spine + 1,
             static_cast<unsigned>(builds[i].pages));
    writeLine(line, kEventStallMs);
  }
}

void serviceIdle() {
  if (!idleWait.active) return;
  const uint32_t now = millis();
  const char* busy = activityManager.busyReason(/*includeBackground=*/!idleWait.screenOnly);
  if (busy) {
    idleWait.quiet = false;
    idleWait.lastBusy = busy;
  } else if (!idleWait.quiet) {
    idleWait.quiet = true;
    idleWait.quietSinceMs = now;
  }
  if (idleWait.quiet && now - idleWait.quietSinceMs >= kIdleQuietMs) {
    idleWait.active = false;
    replyf("IDLE:%lu", static_cast<unsigned long>(now - idleWait.startMs));
  } else if (now - idleWait.startMs >= idleWait.timeoutMs) {
    idleWait.active = false;
    replyf("IDLE_TIMEOUT:%s", busy ? busy : idleWait.lastBusy);
  }
}

}  // namespace

void begin(GfxRenderer& renderer) { testRenderer = &renderer; }

void handleLine(const std::string_view line) {
  constexpr std::string_view kPrefix = "CMD:";
  if (line.substr(0, kPrefix.size()) != kPrefix) return;
  const std::string_view command = trim(line.substr(kPrefix.size()));
  std::string_view name;
  std::string_view arg;
  splitHead(command, name, arg);
  for (const Command& entry : kCommands) {
    if (name == entry.name) {
      entry.run(arg);
      return;
    }
  }
  writeLine(std::string("CMD_ERROR:unknown command ") + std::string(command));
}

void service() {
  serviceEvents();
  serviceIdle();
}

void sendScreenshot(const uint8_t* frame, const uint32_t size, const int width, const int height, const bool busy) {
  if (busy) {
    writeLine("SCREENSHOT_ERROR:busy");
    return;
  }
  if (!frame) {
    // Framebuffers are released during the web server session — nothing to send.
    writeLine("SCREENSHOT_ERROR:framebuffer released");
    return;
  }
  replyf("SCREENSHOT_START:%d:%d:%lu", width, height, static_cast<unsigned long>(size));
  // ~48 KB in one go is exactly the sustained write that trips the short-write behaviour, so the
  // END marker must not be sent if the payload was cut short: a truncated frame with a valid
  // trailer looks complete to the host. 15 s without progress matches SerialTransferDevice.
  if (writeAll(frame, size, 15000)) {
    writeLine("SCREENSHOT_END");
  } else {
    writeLine("SCREENSHOT_ERROR:transfer stalled");
  }
}

void notePopup(const char* text) { noteText(popupProbe, -1, text); }

void noteSelection(const int index, const char* label) { noteText(selectionProbe, index, label); }

void noteBuildDone(const int spineIndex, const int pageCount) {
  portENTER_CRITICAL(&probeMux);
  const uint8_t next = static_cast<uint8_t>((buildTail + 1) % kBuildRing);
  if (next != buildHead) {
    buildRing[buildTail] = BuildDone{static_cast<int16_t>(spineIndex), static_cast<uint16_t>(pageCount)};
    buildTail = next;
  }
  portEXIT_CRITICAL(&probeMux);
}

PowerRequest takePowerRequest() {
  const PowerRequest request = powerRequest;
  powerRequest = PowerRequest::None;
  return request;
}

}  // namespace SerialTestApi
