#include "SerialTestApi.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>

#include "BootDiagnostics.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/home/LibraryTab.h"
#include "util/ScreenshotUtil.h"
#include "util/SerialCommandGrammar.h"

#if SERIAL_TEST_API
#include <GfxRenderer.h>
#include <Memory.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#include "ButtonEventManager.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "SettingsByKey.h"
#include "TouchUi.h"  // CP_TOUCH_UI
#include "util/Dictionary.h"
#include "util/IdleGate.h"
#endif

namespace SerialTestApi {

namespace {

using SerialCommandGrammar::appendField;

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

bool writeText(const std::string_view text, const unsigned long stallMs) {
  return writeAll(reinterpret_cast<const uint8_t*>(text.data()), text.size(), stallMs);
}

// A reply waits up to 2 s for the host to read it.
constexpr unsigned long kReplyStallMs = 2000;

// One reply line: `head`, then `tail` (an argument echoed back), then the line end.
void reply(const std::string_view head, const std::string_view tail = {}, const unsigned long stallMs = kReplyStallMs) {
  if (writeText(head, stallMs) && writeText(tail, stallMs)) writeText("\n", stallMs);
}

void replyf(const char* format, ...) {
  char line[96];
  va_list args;
  va_start(args, format);
  const int n = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (n > 0) reply(std::string_view(line, std::min(static_cast<size_t>(n), sizeof(line) - 1)));
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

#if SERIAL_TEST_API
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

GfxRenderer* testRenderer = nullptr;
bool eventsOn = false;
uint32_t reportedActivity = 0;
uint32_t reportedPopup = 0;
// An event line (sent unasked once EVENTS:ON) waits only 50 ms, so a host that enabled events and
// went away cannot hold the loop up.
constexpr unsigned long kEventStallMs = 50;

IdleGate idleGate;
bool idleScreenOnly = false;
constexpr uint32_t kIdleDefaultTimeoutMs = 10000;
constexpr uint32_t kIdleMaxTimeoutMs = 600000;

// The last command meant to change the screen, which CMD:IDLE waits to see through (IdleGate).
// Input: a press or touch, handed over once HalGPIO has delivered it and the classifier is done
// with it. Screen: an ACT, handed over once the activity on top has changed.
enum class Effect : uint8_t { Input, Screen };
Effect effect = Effect::Input;
uint32_t effectActivityBase = 0;

void noteEffect(const Effect kind) {
  effect = kind;
  effectActivityBase = activityManager.activityChangeCount();
  idleGate.noteEffect();
}

bool effectHandled() {
  switch (effect) {
    case Effect::Input:
      return !gpio.injectedInputPending() && !globalButtonEvents().isMidGesture();
    case Effect::Screen:
      return activityManager.activityChangeCount() != effectActivityBase;
  }
  return true;
}

PowerRequest powerRequest = PowerRequest::None;
#else
enum class Effect : uint8_t { Input, Screen };
inline void noteEffect(Effect) {}
#endif  // SERIAL_TEST_API

// ---- BTN ------------------------------------------------------------------------------------

void cmdButton(const std::string_view arg) {
  using SerialCommandGrammar::PressKind;
  std::string_view name;
  PressKind kind = PressKind::Short;
  if (!SerialCommandGrammar::parseButtonArg(arg, name, kind)) {
    reply("BTN_ERROR:bad press ", arg);
    return;
  }
  struct NamedButton {
    const char* name;
    uint8_t index;
  };
  static constexpr NamedButton kButtons[] = {{"BACK", HalGPIO::BTN_BACK},  {"CONFIRM", HalGPIO::BTN_CONFIRM},
                                             {"LEFT", HalGPIO::BTN_LEFT},  {"RIGHT", HalGPIO::BTN_RIGHT},
                                             {"UP", HalGPIO::BTN_UP},      {"DOWN", HalGPIO::BTN_DOWN},
#if SERIAL_TEST_API
                                             {"POWER", HalGPIO::BTN_POWER}
#endif
  };
  for (const NamedButton& button : kButtons) {
    if (name != button.name) continue;
    switch (kind) {
      case PressKind::Short:
        gpio.injectPress(button.index);
        break;
      case PressKind::Long:
#if SERIAL_TEST_API
        if (button.index == HalGPIO::BTN_POWER) {
          // Holding power sleeps the device, and that is main.cpp's hold timer reading the live
          // pin, which an injected press never reaches. Do what the hold does.
          powerRequest = PowerRequest::Sleep;
          reply("BTN_OK");
          return;
        }
#endif
        gpio.injectPress(button.index, /*longPress=*/true);
        break;
      case PressKind::Double: {
#if SERIAL_TEST_API
        // Two taps inside ButtonEventManager::DOUBLE_WINDOW_MS (300 ms), dated apart so the
        // classifier sees two presses. A button with no double action configured takes them as two
        // short presses, as it would from a finger.
        constexpr uint32_t kTapGapMs = 120;
        const uint32_t now = millis();
        gpio.injectPress(button.index, false, now > kTapGapMs ? now - kTapGapMs : 0);
        gpio.injectPress(button.index, false, now);
        break;
#else
        reply("BTN_ERROR:bad press ", arg);
        return;
#endif
      }
    }
    noteEffect(Effect::Input);
    reply("BTN_OK");
    return;
  }
  reply("BTN_ERROR:unknown button ", name);
}

// ---- ACT ------------------------------------------------------------------------------------

bool openReader(const std::string_view arg) {
#if SERIAL_TEST_API
  SerialCommandGrammar::ReaderTarget target;
  if (!SerialCommandGrammar::parseReaderTarget(arg, target)) {
    reply("ACT_ERROR:bad position ", arg);
    return false;
  }
  std::string path(target.path);
#else
  std::string path(arg);
#endif
  if (!Storage.exists(path.c_str())) {
    reply("ACT_ERROR:no such file ", path);
    return false;
  }
#if SERIAL_TEST_API
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
#endif
  activityManager.goToReader(std::move(path));
  return true;
}

// A keyword's position in `names`, or -1.
int indexOf(const std::string_view name, const char* const* names, const int count) {
  for (int i = 0; i < count; ++i) {
    if (name == names[i]) return i;
  }
  return -1;
}

#if SERIAL_TEST_API
// CMD:ACT:DICT:<word>. The dictionary screen's lookup without the screen: open the configured
// dictionary, build its index if it needs one, look the word up, and say what came of it.
void lookUpWord(const std::string_view wordView) {
  if (wordView.empty()) {
    reply("DICT_ERROR:no word");
    return;
  }
  const std::string text(wordView);  // not `word`: Arduino.h typedefs that
  const char* dictionaryName = SETTINGS.dictionaryName;
  const char* outcome = "no_dictionary";
  std::string definition;
  std::string headword;
  bool found = false;
  if (dictionaryName[0] != '\0') {
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
  std::string line = "DICT:result=";
  line += outcome;
  appendField(line, "dict", dictionaryName);
  appendField(line, "word", text);
  if (found) {
    appendField(line, "headword", headword);
    appendField(line, "bytes", static_cast<long>(definition.size()));
  }
  reply(line);
}
#endif  // SERIAL_TEST_API

void cmdActivity(const std::string_view arg) {
  std::string_view target;
  std::string_view rest;
  splitHead(arg, target, rest);
  static constexpr const char* kTargets[] = {"HOME",    "READER", "SETTINGS",      "BOOKMARKS",   "FILES",
                                             "LIBRARY", "DICT",   "FILE_TRANSFER", "USB_TRANSFER"};
#if SERIAL_TEST_API
  constexpr int kTargetCount = 9;
#else
  constexpr int kTargetCount = 6;  // the original targets; the rest come with SERIAL_TEST_API
#endif
  switch (indexOf(target, kTargets, kTargetCount)) {
    case 0:
      activityManager.goHome();
      break;
    case 1:
      if (!openReader(rest)) return;
      break;
    case 2: {
      int tab = 0;
#if SERIAL_TEST_API
      // SettingsActivity's tabs, in its categoryNames order, or their numbers.
      static constexpr const char* kTabs[] = {"DISPLAY", "READER", "CONTROLS", "SYSTEM"};
      if (!rest.empty()) {
        tab = indexOf(rest, kTabs, 4);
        if (tab < 0 && (!SerialCommandGrammar::parseInt(rest, tab) || tab < 0 || tab > 3)) {
          reply("ACT_ERROR:unknown settings tab ", rest);
          return;
        }
      }
#endif
      activityManager.goToSettings(tab);
      break;
    }
    case 3:
      activityManager.goToGlobalBookmarks();
      break;
    case 4:
      activityManager.goToLibrary(LibraryTab::Books, std::string(rest));
      break;
    case 5: {
      // LibraryTab's order.
      static constexpr const char* kTabs[] = {"BOOKS", "RECENT", "NEW", "AUTHORS"};
      const int tab = rest.empty() ? 0 : indexOf(rest, kTabs, 4);
      if (tab < 0) {
        reply("ACT_ERROR:unknown library tab ", rest);
        return;
      }
      activityManager.goToLibrary(static_cast<LibraryTab>(tab));
      break;
    }
#if SERIAL_TEST_API
    case 6:
      lookUpWord(rest);
      return;
    case 7:
      activityManager.goToFileTransfer();
      break;
    case 8:
      // Leaving the transfer screen reboots (SerialTransferActivity::onExit), so replacing it with
      // a fresh one would land on Home: already there is already done.
      if (std::strcmp(activityManager.currentActivityName(), "SerialTransfer") == 0) {
        reply("ACT_OK");
        return;
      }
      activityManager.goToSerialTransfer();
      break;
#endif
    default:
      reply("ACT_ERROR:unknown activity ", target);
      return;
  }
  noteEffect(Effect::Screen);
  reply("ACT_OK");
}

// ---- STATE ----------------------------------------------------------------------------------

void cmdState(std::string_view /*arg*/) {
  std::string state = "STATE:";
  state.reserve(320);
  {
    RenderLock lock;  // the activity's state is also read by the render task
    activityManager.describeState(state);
  }
#if SERIAL_TEST_API
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
#endif
  reply(state);
}

#if SERIAL_TEST_API
// ---- IDLE / EVENTS --------------------------------------------------------------------------

void cmdIdle(const std::string_view arg) {
  // [<timeout_ms>][:SCREEN]
  std::string_view first;
  std::string_view second;
  splitHead(arg, first, second);
  int timeout = static_cast<int>(kIdleDefaultTimeoutMs);
  if (first == "SCREEN") {
    second = first;
  } else if (!first.empty() && (!SerialCommandGrammar::parseInt(first, timeout) || timeout <= 0)) {
    reply("IDLE_ERROR:bad timeout ", first);
    return;
  }
  if (!second.empty() && second != "SCREEN") {
    reply("IDLE_ERROR:bad mode ", second);
    return;
  }
  idleScreenOnly = !second.empty();
  idleGate.begin(millis(), std::min(static_cast<uint32_t>(timeout), kIdleMaxTimeoutMs));
}

void cmdEvents(const std::string_view arg) {
  if (arg == "ON" || arg == "OFF") {
    eventsOn = arg == "ON";
  } else if (!arg.empty()) {
    reply("EVENTS_ERROR:expected ON or OFF, got ", arg);
    return;
  }
  replyf("EVENTS_OK:%s", eventsOn ? "on" : "off");
}

// ---- GET / SET ------------------------------------------------------------------------------

// "<key>=<value>[ label=<option label>]" for a GET or SET_OK reply.
void appendSetting(std::string& line, const SettingInfo& s) {
  line += s.key;
  line += '=';
  int number = 0;
  if (SettingsByKey::readNumber(s, number)) {
    char digits[12];
    snprintf(digits, sizeof(digits), "%d", number);
    line += digits;
    if (s.type == SettingType::ENUM && number >= 0 && number < SettingsByKey::optionCount(s)) {
      appendField(line, "label", SettingsByKey::optionLabel(s, static_cast<uint8_t>(number)));
    }
  } else if (s.type != SettingType::STRING) {
    line += "(none)";
  } else if (s.obfuscated) {
    line += "(hidden)";  // credentials never leave the device, as on the web settings page
  } else if (s.stringGetter) {
    SerialCommandGrammar::appendValue(line, s.callStringGetter());
  } else {
    SerialCommandGrammar::appendValue(
        line, s.stringMaxLen > 0 ? reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset : "");
  }
}

// GET and GET:* both go through SettingsByKey::visitKeyed(), one walk with the web settings API.
void replyGetRow(const SettingInfo& s, void* /*ctx*/) {
  if (!SettingsByKey::optionsKnown(s)) {
    // The family the setting names is not among the listed ones: its index and label would be wrong.
    reply("GET_ERROR:font families not loaded on this screen ", s.key);
    return;
  }
  std::string line = "GET:";
  appendSetting(line, s);
  reply(line);
}

void cmdGet(const std::string_view key) {
  if (key == "*") {
    replyf("GET_END:%d", SettingsByKey::visitKeyed({}, &replyGetRow, nullptr));
  } else if (key.empty() || SettingsByKey::visitKeyed(key, &replyGetRow, nullptr) == 0) {
    reply("GET_ERROR:unknown key ", key);
  }
}

struct SetRequest {
  std::string_view value;
  const char* error;     // the reply's head when the write did not happen
  bool errorNamesValue;  // the error is about the value, not the key
};

void writeSetRow(const SettingInfo& s, void* ctx) {
  auto& request = *static_cast<SetRequest*>(ctx);
  SettingsByKey::WriteResult result = SettingsByKey::WriteResult::NotWritable;
  if (!SettingsByKey::optionsKnown(s)) {
    // On the boot straight into USB Transfer the card's font families are not listed: a name would
    // be refused as a bad value, and an index would store whatever it means among the built-ins.
    request.error = "SET_ERROR:font families not loaded on this screen ";
    return;
  }
  if (s.type == SettingType::STRING) {
    result = SettingsByKey::writeString(s, std::string(request.value));
  } else {
    int number = 0;
    bool parsed = SerialCommandGrammar::parseInt(request.value, number);
    // An ENUM row also takes one of its option labels, as GET shows them.
    for (uint8_t i = 0; !parsed && s.type == SettingType::ENUM && i < SettingsByKey::optionCount(s); ++i) {
      if (request.value == SettingsByKey::optionLabel(s, i)) {
        number = i;
        parsed = true;
      }
    }
    if (!parsed) {
      request.error = "SET_ERROR:bad value ";
      request.errorNamesValue = true;
      return;
    }
    result = SettingsByKey::writeNumber(s, number);
  }
  switch (result) {
    case SettingsByKey::WriteResult::Applied:
      request.error = nullptr;
      return;
    case SettingsByKey::WriteResult::OutOfRange:
      request.error = "SET_ERROR:out of range ";
      request.errorNamesValue = true;
      return;
    case SettingsByKey::WriteResult::NotWritable:
      request.error = "SET_ERROR:not writable ";
      return;
  }
}

void replySetRow(const SettingInfo& s, void* /*ctx*/) {
  std::string line = "SET_OK:";
  appendSetting(line, s);
  reply(line);
}

void cmdSet(const std::string_view arg) {
  std::string_view key;
  SetRequest request{{}, "SET_ERROR:unknown key ", false};
  if (!SerialCommandGrammar::splitKeyValue(arg, key, request.value)) {
    reply("SET_ERROR:expected <key>=<value>");
    return;
  }
  SettingsByKey::visitKeyed(key, &writeSetRow, &request);
  if (request.error) {
    reply(request.error, request.errorNamesValue ? request.value : key);
    return;
  }
  // As the web settings page does: settle the settings that depend on others, save, and read the
  // value back so the reply shows what was stored (a dependent setting may have moved it).
  SettingsByKey::commit();
  SettingsByKey::visitKeyed(key, &replySetRow, nullptr);
}

// ---- LOG ------------------------------------------------------------------------------------

void cmdLog(const std::string_view arg) {
  static constexpr const char* kLevelTags[] = {"ERR", "INF", "DBG", "TRC"};
  uint8_t level = 0;
  if (!arg.empty()) {
    if (!SerialCommandGrammar::parseLogLevel(arg, level)) {
      reply("LOG_ERROR:unknown level ", arg);
      return;
    }
    setLogWireLevel(level);
  }
  constexpr int kCompiled = LOG_LEVEL < 3 ? LOG_LEVEL : 3;
  replyf("LOG_OK:level=%s compiled=%s", kLevelTags[std::min<int>(logWireLevel(), 3)], kLevelTags[kCompiled]);
}

// ---- TOUCH / SWIPE --------------------------------------------------------------------------

// CMD:TOUCH:<x>,<y> (2 numbers) or CMD:SWIPE:<x1>,<y1>,<x2>,<y2> (4): logical screen pixels as drawn
// now, mapped to the touch HAL's normalized panel coordinates and queued like a contact. Builds
// for boards without a digitiser (CP_TOUCH_UI 0: X3, X4) only say so.
void injectContact(const std::string_view arg, const size_t count, const char* ok) {
#if CP_TOUCH_UI
  if (!gpio.hasTouch() || !testRenderer) {
    reply("TOUCH_ERROR:no touch panel");
    return;
  }
  int xy[4];
  float n[4];
  bool onScreen = SerialCommandGrammar::parseIntList(arg, xy, count);
  for (size_t i = 0; onScreen && i < count; i += 2) {
    onScreen = xy[i] >= 0 && xy[i + 1] >= 0 && xy[i] < testRenderer->getScreenWidth() &&
               xy[i + 1] < testRenderer->getScreenHeight();
    if (onScreen) testRenderer->logicalToTap(xy[i], xy[i + 1], n[i], n[i + 1]);
  }
  if (!onScreen) {
    reply("TOUCH_ERROR:not on screen ", arg);
    return;
  }
  if (count == 2) {
    gpio.injectTouch(n[0], n[1]);
  } else {
    gpio.injectSwipe(n[0], n[1], n[2], n[3]);
  }
  noteEffect(Effect::Input);
  reply(ok);
#else
  (void)arg;
  (void)count;
  (void)ok;
  reply("TOUCH_ERROR:no touch panel");
#endif
}

void cmdTouch(const std::string_view arg) { injectContact(arg, 2, "TOUCH_OK"); }
void cmdSwipe(const std::string_view arg) { injectContact(arg, 4, "SWIPE_OK"); }

// ---- Power ----------------------------------------------------------------------------------

void cmdSleep(std::string_view /*arg*/) {
  reply("SLEEP_OK");
  powerRequest = PowerRequest::Sleep;
}

void cmdReboot(std::string_view /*arg*/) {
  reply("REBOOT_OK");
  powerRequest = PowerRequest::Reboot;
}
#endif  // SERIAL_TEST_API

void cmdScreenshot(std::string_view /*arg*/) { ScreenshotUtil::request(ScreenshotUtil::Target::Host); }

// On-demand replay of this session's boot summary, for when the monitor was attached too late to
// catch it and the automatic repeats are used up. The summary is log lines; the reply after them
// tells a script it is complete.
void cmdBootLog(std::string_view /*arg*/) {
  BootDiag::logSummary();
  reply("BOOTLOG_OK");
}

void cmdHelp(std::string_view arg);

using Handler = void (*)(std::string_view arg);
struct Command {
  const char* name;
  Handler run;
};
constexpr Command kCommands[] = {
    {"BTN", &cmdButton},      {"ACT", &cmdActivity},  {"STATE", &cmdState}, {"SCREENSHOT", &cmdScreenshot},
    {"BOOTLOG", &cmdBootLog}, {"HELP", &cmdHelp},
#if SERIAL_TEST_API
    {"IDLE", &cmdIdle},       {"EVENTS", &cmdEvents}, {"GET", &cmdGet},     {"SET", &cmdSet},
    {"LOG", &cmdLog},         {"TOUCH", &cmdTouch},   {"SWIPE", &cmdSwipe}, {"SLEEP", &cmdSleep},
    {"REBOOT", &cmdReboot},
#endif
};

// The commands this build has: with SERIAL_TEST_API or without.
void cmdHelp(std::string_view /*arg*/) {
  writeText("HELP:", kReplyStallMs);
  for (const Command& command : kCommands) {
    writeText(" ", kReplyStallMs);
    writeText(command.name, kReplyStallMs);
  }
  writeText("\n", kReplyStallMs);
}

#if SERIAL_TEST_API
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
    reply("EVT:ACT ", activityManager.currentActivityName(), kEventStallMs);
  }
  if (popup.sequence != reportedPopup) {
    reportedPopup = popup.sequence;
    reply("EVT:POPUP ", popup.text, kEventStallMs);
  }
  for (int i = 0; i < buildCount; ++i) {
    char line[48];
    snprintf(line, sizeof(line), "EVT:BUILD_DONE spine=%d pages=%u", builds[i].spine + 1,
             static_cast<unsigned>(builds[i].pages));
    reply(line, {}, kEventStallMs);
  }
}

void serviceIdle() {
  // Nothing to follow most of the time: then the device is not sampled at all, which keeps the
  // reader's lock attempt in hasPendingBackgroundWork() off the ordinary loop pass.
  if (!idleGate.active()) return;
  IdleGate::Sample sample;
  sample.nowMs = millis();
  sample.busy = activityManager.busyReason(/*includeBackground=*/idleGate.waiting() && !idleScreenOnly);
  sample.effectHandled = effectHandled();
  sample.renderPasses = activityManager.renderPassCount();
  switch (idleGate.step(sample)) {
    case IdleGate::Verdict::Waiting:
      break;
    case IdleGate::Verdict::Idle:
      replyf("IDLE:%lu", static_cast<unsigned long>(idleGate.elapsedMs(sample.nowMs)));
      break;
    case IdleGate::Verdict::TimedOut:
      reply("IDLE_TIMEOUT:", idleGate.reason());
      break;
  }
}
#endif  // SERIAL_TEST_API

}  // namespace

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
  reply("CMD_ERROR:unknown command ", command);
}

void sendScreenshot(const uint8_t* frame, const uint32_t size, const int width, const int height, const bool busy) {
  if (busy) {
    reply("SCREENSHOT_ERROR:busy");
    return;
  }
  if (!frame) {
    // Framebuffers are released during the web server session — nothing to send.
    reply("SCREENSHOT_ERROR:framebuffer released");
    return;
  }
  replyf("SCREENSHOT_START:%d:%d:%lu", width, height, static_cast<unsigned long>(size));
  // ~48 KB in one go is exactly the sustained write that trips the short-write behaviour, so the
  // END marker must not be sent if the payload was cut short: a truncated frame with a valid
  // trailer looks complete to the host. 15 s without progress matches SerialTransferDevice.
  if (writeAll(frame, size, 15000)) {
    reply("SCREENSHOT_END");
  } else {
    reply("SCREENSHOT_ERROR:transfer stalled");
  }
}

#if SERIAL_TEST_API
void begin(GfxRenderer& renderer) { testRenderer = &renderer; }

void service() {
  serviceEvents();
  serviceIdle();
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
#endif  // SERIAL_TEST_API

}  // namespace SerialTestApi
