#include "SerialControl.h"

#ifdef ENABLE_SERIAL_LOG
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <SerialInput.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iterator>
#ifdef SIMULATOR
#include <chrono>
#else
#include <esp_random.h>
#endif

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"

extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

namespace {
char line[128];
uint8_t lineLength = 0;
bool discarding = false;
uint32_t lastByteAt = 0;
uint32_t bootId = 0;
uint32_t lastId = 0;
uint32_t activeId = 0;

struct ButtonName {
  const char* name;
  MappedInputManager::Button button;
};
static constexpr ButtonName BUTTONS[] = {
    {"BACK", MappedInputManager::Button::Back},
    {"CONFIRM", MappedInputManager::Button::Confirm},
    {"LEFT", MappedInputManager::Button::Left},
    {"RIGHT", MappedInputManager::Button::Right},
    {"UP", MappedInputManager::Button::Up},
    {"DOWN", MappedInputManager::Button::Down},
    {"NAV_NEXT", MappedInputManager::Button::NavNext},
    {"NAV_PREVIOUS", MappedInputManager::Button::NavPrevious},
    {"PAGE_FORWARD", MappedInputManager::Button::PageForward},
    {"PAGE_BACK", MappedInputManager::Button::PageBack},
};

bool number(const char* text, uint32_t& value) {
  if (!text || !*text) return false;
  for (const char* c = text; *c; ++c)
    if (*c < '0' || *c > '9') return false;
  errno = 0;
  const unsigned long parsed = strtoul(text, nullptr, 10);
  if (errno || parsed > UINT32_MAX) return false;
  value = parsed;
  return true;
}

void error(uint32_t id, const char* reason) {
  LOG_INF("CTL", "id=%lu boot=%lu event=ERROR reason=%s", static_cast<unsigned long>(id),
          static_cast<unsigned long>(bootId), reason);
}

void state(uint32_t id) {
  ActivityManager::ControlState snapshot;
  activityManager.getControlState(snapshot);
  LOG_INF("CTL",
          "id=%lu boot=%lu event=STATE available=%d busy=%d activity=%s requested=%lu completed=%lu "
          "orientation=%u reader=%u spine=%d page=%d pages=%d",
          static_cast<unsigned long>(id), static_cast<unsigned long>(bootId), snapshot.available,
          snapshot.busy || serialInput.active(), snapshot.available ? snapshot.activity : "unknown",
          static_cast<unsigned long>(snapshot.requested), static_cast<unsigned long>(snapshot.completed),
          snapshot.orientation, static_cast<unsigned int>(snapshot.reader.readerType), snapshot.reader.spineIndex,
          snapshot.reader.currentPage, snapshot.reader.totalPages);
}

void screenshot() {
  if (serialInput.active() || RenderLock::peek() || activityManager.requiresExclusiveStorageLoop()) {
    error(0, "BUSY");
    return;
  }
  RenderLock lock;
  if (!renderer.hasFrameBuffer()) {
    error(0, "NO_FRAMEBUFFER");
    return;
  }
  unsigned int rotation = 0;
  switch (renderer.getOrientation()) {
    case GfxRenderer::Portrait:
      rotation = 90;
      break;
    case GfxRenderer::LandscapeClockwise:
      rotation = 180;
      break;
    case GfxRenderer::PortraitInverted:
      rotation = 270;
      break;
    case GfxRenderer::LandscapeCounterClockwise:
      break;
  }
  const uint32_t size = display.getBufferSize();
  // Physical bytes only. The Python monitor performs rotation and output inversion.
  logSerial.printf("SCREENSHOT_META:1:%u:%u:%u:%u:%u\n", display.getDisplayWidth(), display.getDisplayHeight(),
                   display.getDisplayWidthBytes(), rotation, display.isInverted() ? 1u : 0u);
  logSerial.printf("SCREENSHOT_START:%lu\n", static_cast<unsigned long>(size));
  logSerial.write(display.getFrameBuffer(), size);
  logSerial.printf("SCREENSHOT_END\n");
}

void dispatch() {
  if (strncmp(line, "CMD:", 4) != 0) return;
  char* context = nullptr;
  const char* verb = strtok_r(line + 4, " \t", &context);
  if (!verb) {
    error(0, "SYNTAX");
    return;
  }
  const char* idText = strtok_r(nullptr, " \t", &context);
  if (strcmp(verb, "SCREENSHOT") == 0 && !idText) {
    screenshot();
    return;
  }
  uint32_t id = 0;
  const bool discovery = strcmp(verb, "INFO") == 0;
  if (!number(idText, id) || (id == 0 && !discovery)) {
    error(0, "ID_REQUIRED");
    return;
  }
  if (id != 0) {
    if (id <= lastId) {
      error(id, "STALE_ID");
      return;
    }
    lastId = id;
  }
  const char* arg = strtok_r(nullptr, " \t", &context);
  const char* durationText = strtok_r(nullptr, " \t", &context);
  if (strtok_r(nullptr, " \t", &context)) {
    error(id, "SYNTAX");
    return;
  }
  if (strcmp(verb, "PRESS") == 0) {
    uint32_t duration = 80;
    if (!arg || (durationText && !number(durationText, duration)) || duration < 20 || duration > 2000) {
      error(id, "SYNTAX");
      return;
    }
    ActivityManager::ControlState snapshot;
    activityManager.getControlState(snapshot);
    if (serialInput.active() || snapshot.busy || !snapshot.available || gpio.physicalInputActive()) {
      error(id, "BUSY");
      return;
    }
    const auto* entry = std::find_if(std::begin(BUTTONS), std::end(BUTTONS),
                                     [arg](const ButtonName& candidate) { return strcmp(arg, candidate.name) == 0; });
    const int physical = entry == std::end(BUTTONS) ? -1 : mappedInputManager.controlButton(entry->button);
    if (physical < 0 || physical == HalGPIO::BTN_POWER) {
      error(id, "UNSUPPORTED_BUTTON");
      return;
    }
    serialInput.start(static_cast<uint8_t>(physical), duration);
    activeId = id;
    LOG_INF("CTL", "id=%lu boot=%lu event=ACCEPTED button=%s hold_ms=%lu", static_cast<unsigned long>(id),
            static_cast<unsigned long>(bootId), arg, static_cast<unsigned long>(duration));
    return;
  }
  if (arg || durationText) {
    error(id, "SYNTAX");
    return;
  }
  if (strcmp(verb, "INFO") == 0) {
    LOG_INF("CTL", "id=%lu boot=%lu event=INFO protocol=1 last_id=%lu min_hold=20 max_hold=2000",
            static_cast<unsigned long>(id), static_cast<unsigned long>(bootId), static_cast<unsigned long>(lastId));
    for (const auto& entry : BUTTONS) {
      const int physical = mappedInputManager.controlButton(entry.button);
      if (physical >= 0 && physical != HalGPIO::BTN_POWER)
        LOG_INF("CTL", "id=%lu boot=%lu event=BUTTON name=%s", static_cast<unsigned long>(id),
                static_cast<unsigned long>(bootId), entry.name);
    }
  } else if (strcmp(verb, "STATE") == 0) {
    state(id);
  } else if (strcmp(verb, "CANCEL") == 0) {
    if (activeId) {
      LOG_INF("CTL", "id=%lu boot=%lu event=CANCELLED", static_cast<unsigned long>(activeId),
              static_cast<unsigned long>(bootId));
    }
    serialInput.cancel();
    activeId = 0;
    LOG_INF("CTL", "id=%lu boot=%lu event=CANCELLED", static_cast<unsigned long>(id),
            static_cast<unsigned long>(bootId));
  } else
    error(id, "UNKNOWN_COMMAND");
}
}  // namespace

void SerialControl::beginFrame() {
  if (!bootId) {
#ifdef SIMULATOR
    bootId = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#else
    bootId = esp_random();
#endif
    if (!bootId) bootId = 1;
  }
  serialInput.beginFrame(millis());
}

void SerialControl::poll() {
  if (activeId && (gpio.physicalInputActive() || activityManager.requiresExclusiveStorageLoop())) {
    serialInput.cancel();
    error(activeId, "INTERRUPTED");
    activeId = 0;
  }
  if (lineLength && static_cast<uint32_t>(millis() - lastByteAt) > 1000) {
    lineLength = 0;
    discarding = true;
    error(0, "PARTIAL_TIMEOUT");
  }
  for (unsigned int budget = 0; budget < 64 && logSerial.available() > 0; ++budget) {
    const int c = logSerial.read();
    lastByteAt = millis();
    if (c == '\r') continue;
    if (c == '\n') {
      if (!discarding) {
        line[lineLength] = '\0';
        dispatch();
      }
      lineLength = 0;
      discarding = false;
      return;
    }
    if (discarding) continue;
    if (c < 32 || c > 126 || lineLength == sizeof(line) - 1) {
      discarding = true;
      lineLength = 0;
      error(0, "INVALID_LINE");
    } else
      line[lineLength++] = static_cast<char>(c);
  }
}

void SerialControl::endFrame() {
  if (activeId && serialInput.released()) {
    LOG_INF("CTL", "id=%lu boot=%lu event=INPUT_DONE held_ms=%lu", static_cast<unsigned long>(activeId),
            static_cast<unsigned long>(bootId), static_cast<unsigned long>(serialInput.heldMs(millis())));
    activeId = 0;
  }
}
#endif
