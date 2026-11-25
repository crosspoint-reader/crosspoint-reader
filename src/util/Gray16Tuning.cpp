#include "Gray16Tuning.h"

#if CROSSPOINT_GRAY16_TUNING
#include <Arduino.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "activities/RenderLock.h"

namespace {

uint8_t levelPaths[16][3];
bool pathsLoaded = false;
// Gray frames already run on a tuned table (GfxRenderer's kGrayUiLevels).
bool tuningOn = true;

bool loadPaths() {
  if (!pathsLoaded) pathsLoaded = display.getGray16Levels(levelPaths);
  if (!pathsLoaded) logSerial.printf("GRAY16: this panel has no level tuning\n");
  return pathsLoaded;
}

void dumpPaths() {
  logSerial.printf("GRAY16: %s; level: black pull clear\n", tuningOn ? "on" : "off (built-in levels drawn)");
  logSerial.printf("GRAY16:  0: top-up black %u white %u, drawing %s\n", levelPaths[0][0], levelPaths[0][1],
                   levelPaths[0][2] ? "overlaps clearing" : "follows clearing");
  for (int level = 1; level < 16; ++level) {
    logSerial.printf("GRAY16: %2d: %u %u %u\n", level, levelPaths[level][0], levelPaths[level][1],
                     levelPaths[level][2]);
  }
}

// Builds the table from the current numbers; the display refuses one that would be too long.
bool applyPaths() {
  RenderLock lock;
  const bool applied = display.setGray16Levels(levelPaths);
  logSerial.printf("GRAY16: %s\n", applied ? "applied" : "refused (the frame would be too long)");
  return applied;
}

// Draws 16 bars across the panel's long side in each half: upper[i] and
// lower[i] are the levels of bar i.
void showBars(const uint8_t upper[16], const uint8_t lower[16], bool fast);

void showPattern(const int pattern, const bool fast) {
  uint8_t upper[16], lower[16];
  for (int bar = 0; bar < 16; ++bar) {
    const uint8_t ascending = static_cast<uint8_t>(pattern == 2 ? 0 : bar);
    const uint8_t descending = static_cast<uint8_t>(pattern == 2 ? 0 : 15 - bar);
    upper[bar] = pattern == 1 ? descending : ascending;
    lower[bar] = pattern == 1 ? ascending : descending;
  }
  showBars(upper, lower, fast);
  logSerial.printf("GRAY16: pattern %d\n", pattern);
}

void showBars(const uint8_t upper[16], const uint8_t lower[16], const bool fast) {
  const int width = display.getDisplayWidth();
  const int height = display.getDisplayHeight();
  const size_t stride = static_cast<size_t>(width) / 2;
  auto image = HalMemory::allocatePsram(stride * height);
  if (!image) {
    logSerial.printf("GRAY16: no memory for the pattern\n");
    return;
  }
  const int barWidth = width / 16;
  for (int y = 0; y < height; ++y) {
    uint8_t* row = image.get() + static_cast<size_t>(y) * stride;
    const uint8_t* levels = y < height / 2 ? upper : lower;
    for (int x = 0; x < width; x += 2) {
      const int level = levels[x / barWidth > 15 ? 15 : x / barWidth] & 0x0F;
      row[x / 2] = static_cast<uint8_t>((level << 4) | level);
    }
  }
  RenderLock lock;
  // As for a render: the panel's line feed cannot keep up at the idle clock.
  HalPowerManager::Lock powerLock;
  if (!fast) {
    // "Clean" is the pattern drawn from white: a white frame first, so every
    // bar is reached by its from-white numbers alone.
    auto white = HalMemory::allocatePsram(stride * height);
    if (white) {
      memset(white.get(), 0, stride * height);
      display.displayGray16Image(white.get(), true);
    }
  }
  const bool shown = display.displayGray16Image(image.get(), true);
  logSerial.printf("GRAY16: %s\n", shown ? "shown" : "not shown");
}

int hexDigit(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool handleGray16Command(const String& command) {
  if (!command.startsWith("GRAY16")) return false;
  char verb[8] = {};
  int a = 0, b = 0, c = 0, d = 0;
  const int fields = sscanf(command.c_str(), "GRAY16 %7s %d %d %d %d", verb, &a, &b, &c, &d);
  if (fields < 1 || !loadPaths()) return true;

  // BARS S|C <32 hex digits>: a level for each bar, the upper half's 16 then the lower half's.
  char how = 0;
  char digits[34] = {};
  if (strcmp(verb, "BARS") == 0) {
    uint8_t levels[32];
    bool valid = sscanf(command.c_str(), "GRAY16 BARS %c %33s", &how, digits) == 2 && strlen(digits) == 32 &&
                 (how == 'S' || how == 'C');
    for (int i = 0; valid && i < 32; ++i) {
      const int level = hexDigit(digits[i]);
      valid = level >= 0;
      levels[i] = static_cast<uint8_t>(level);
    }
    if (valid) {
      showBars(levels, levels + 16, how == 'S');
    } else {
      logSerial.printf("GRAY16: BARS S|C <32 hex digits>\n");
    }
    return true;
  }

  if (strcmp(verb, "DUMP") == 0) {
    dumpPaths();
  } else if (strcmp(verb, "ON") == 0) {
    tuningOn = applyPaths();
  } else if (strcmp(verb, "OFF") == 0) {
    RenderLock lock;
    display.setGray16Levels(nullptr);
    tuningOn = false;
    logSerial.printf("GRAY16: off\n");
  } else if (strcmp(verb, "SET") == 0 && fields == 5 && a >= 0 && a <= 15 && b >= 0 && b <= 63 && c >= 0 && c <= 63 &&
             d >= 0 && d <= 63) {
    const uint8_t previous[3] = {levelPaths[a][0], levelPaths[a][1], levelPaths[a][2]};
    levelPaths[a][0] = static_cast<uint8_t>(b);
    levelPaths[a][1] = static_cast<uint8_t>(c);
    levelPaths[a][2] = static_cast<uint8_t>(d);
    // A refused table leaves the old one in use, so keep the old numbers too.
    if (tuningOn && !applyPaths()) memcpy(levelPaths[a], previous, sizeof(previous));
  } else if ((strcmp(verb, "SHOW") == 0 || strcmp(verb, "CLEAN") == 0) && fields == 2 && a >= 0 && a <= 2) {
    showPattern(a, verb[0] == 'S');
  } else {
    logSerial.printf("GRAY16: DUMP | ON | OFF | SET l b p c | SHOW k | CLEAN k | BARS S|C hex32\n");
  }
  return true;
}
#endif
