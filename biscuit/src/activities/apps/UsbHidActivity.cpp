#include "UsbHidActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <cstring>
#include <cstdlib>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

static constexpr const char* DUCKY_DIR = "/biscuit/ducky";
static constexpr int MAX_PREVIEW_LINES = 12;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void UsbHidActivity::onEnter() {
  Activity::onEnter();
  state = FILE_SELECT;
  fileIndex = 0;
  duckyFiles.clear();
  script.clear();
  previewLines.clear();
  previewScroll = 0;
  currentLine = 0;
  waiting = false;
  totalKeystrokes = 0;
  errorMsg[0] = '\0';
  loadDuckyFiles();
  requestUpdate();
}

void UsbHidActivity::onExit() {
  releaseAllKeys();
  deinitUsbHid();
  script.clear();
  duckyFiles.clear();
  previewLines.clear();
  Activity::onExit();
}

// ---------------------------------------------------------------------------
// File loading
// ---------------------------------------------------------------------------

void UsbHidActivity::loadDuckyFiles() {
  duckyFiles.clear();
  Storage.mkdir(DUCKY_DIR);
  HalFile dir = Storage.open(DUCKY_DIR);
  if (!dir) return;

  HalFile entry;
  while ((entry = dir.openNextFile())) {
    char nameBuf[64];
    entry.getName(nameBuf, sizeof(nameBuf));
    entry.close();

    int len = static_cast<int>(strlen(nameBuf));
    // Accept only .txt files
    if (len > 4 && nameBuf[len - 4] == '.' &&
        (nameBuf[len - 3] == 't' || nameBuf[len - 3] == 'T') &&
        (nameBuf[len - 2] == 'x' || nameBuf[len - 2] == 'X') &&
        (nameBuf[len - 1] == 't' || nameBuf[len - 1] == 'T')) {
      if (duckyFiles.size() < 64) {
        DuckyFile df;
        strncpy(df.name, nameBuf, sizeof(df.name) - 1);
        df.name[sizeof(df.name) - 1] = '\0';
        duckyFiles.push_back(df);
      }
    }
  }
  dir.close();
}

bool UsbHidActivity::parseScript(const char* filename) {
  script.clear();
  previewLines.clear();

  char path[96];
  snprintf(path, sizeof(path), "%s/%s", DUCKY_DIR, filename);

  HalFile file = Storage.open(path);
  if (!file) {
    snprintf(errorMsg, sizeof(errorMsg), "Cannot open: %s", filename);
    return false;
  }

  char lineBuf[256];
  while (file.available()) {
    int len = 0;
    while (file.available() && len < (int)sizeof(lineBuf) - 1) {
      char c = static_cast<char>(file.read());
      if (c == '\n') break;
      if (c != '\r') lineBuf[len++] = c;
    }
    lineBuf[len] = '\0';

    // Strip trailing whitespace
    while (len > 0 && (lineBuf[len - 1] == ' ' || lineBuf[len - 1] == '\t')) {
      lineBuf[--len] = '\0';
    }

    if (len == 0) continue;

    // Collect preview lines (raw text, capped)
    if ((int)previewLines.size() < MAX_PREVIEW_LINES) {
      previewLines.push_back(std::string(lineBuf));
    }

    script.push_back(parseLine(lineBuf));
  }
  file.close();
  return true;
}

// ---------------------------------------------------------------------------
// DuckyScript parser
// ---------------------------------------------------------------------------

UsbHidActivity::DuckyLine UsbHidActivity::parseLine(const char* line) const {
  DuckyLine dl;

  // Skip leading whitespace
  while (*line == ' ' || *line == '\t') line++;

  if (strncmp(line, "REM", 3) == 0) {
    dl.cmd = DuckyLine::REM;
  } else if (strncmp(line, "STRING ", 7) == 0) {
    dl.cmd = DuckyLine::STRING;
    strncpy(dl.payload, line + 7, sizeof(dl.payload) - 1);
  } else if (strncmp(line, "DELAY ", 6) == 0) {
    dl.cmd = DuckyLine::DELAY;
    dl.value = atoi(line + 6);
  } else if (strcmp(line, "ENTER") == 0 || strcmp(line, "RETURN") == 0) {
    dl.cmd = DuckyLine::ENTER;
  } else if (strcmp(line, "TAB") == 0) {
    dl.cmd = DuckyLine::TAB;
  } else if (strcmp(line, "SPACE") == 0) {
    dl.cmd = DuckyLine::SPACE;
  } else if (strcmp(line, "BACKSPACE") == 0 || strcmp(line, "DELETE") == 0) {
    dl.cmd = DuckyLine::BACKSPACE;
  } else if (strcmp(line, "ESCAPE") == 0 || strcmp(line, "ESC") == 0) {
    dl.cmd = DuckyLine::ESCAPE;
  } else if (strcmp(line, "UP") == 0) {
    dl.cmd = DuckyLine::ARROW_UP;
  } else if (strcmp(line, "DOWN") == 0) {
    dl.cmd = DuckyLine::ARROW_DOWN;
  } else if (strcmp(line, "LEFT") == 0) {
    dl.cmd = DuckyLine::ARROW_LEFT;
  } else if (strcmp(line, "RIGHT") == 0) {
    dl.cmd = DuckyLine::ARROW_RIGHT;
  } else if (line[0] == 'F' && line[1] >= '1' && line[1] <= '9' &&
             (line[2] == '\0' || (line[1] == '1' && line[2] >= '0' && line[2] <= '2' && line[3] == '\0'))) {
    // F1-F12
    dl.cmd = DuckyLine::F_KEY;
    dl.value = atoi(line + 1);
  } else if (strncmp(line, "GUI ", 4) == 0) {
    dl.cmd = DuckyLine::GUI_KEY;
    strncpy(dl.payload, line + 4, sizeof(dl.payload) - 1);
  } else if (strncmp(line, "WINDOWS ", 8) == 0) {
    dl.cmd = DuckyLine::GUI_KEY;
    strncpy(dl.payload, line + 8, sizeof(dl.payload) - 1);
  } else if (strcmp(line, "GUI") == 0 || strcmp(line, "WINDOWS") == 0) {
    dl.cmd = DuckyLine::GUI_KEY;
    dl.payload[0] = '\0';
  } else if (strncmp(line, "ALT ", 4) == 0) {
    dl.cmd = DuckyLine::ALT_KEY;
    strncpy(dl.payload, line + 4, sizeof(dl.payload) - 1);
  } else if (strcmp(line, "ALT") == 0) {
    dl.cmd = DuckyLine::ALT_KEY;
    dl.payload[0] = '\0';
  } else if (strncmp(line, "CTRL ", 5) == 0) {
    dl.cmd = DuckyLine::CTRL_KEY;
    strncpy(dl.payload, line + 5, sizeof(dl.payload) - 1);
  } else if (strncmp(line, "CONTROL ", 8) == 0) {
    dl.cmd = DuckyLine::CTRL_KEY;
    strncpy(dl.payload, line + 8, sizeof(dl.payload) - 1);
  } else if (strcmp(line, "CTRL") == 0 || strcmp(line, "CONTROL") == 0) {
    dl.cmd = DuckyLine::CTRL_KEY;
    dl.payload[0] = '\0';
  } else if (strncmp(line, "SHIFT ", 6) == 0) {
    dl.cmd = DuckyLine::SHIFT_KEY;
    strncpy(dl.payload, line + 6, sizeof(dl.payload) - 1);
  } else if (strcmp(line, "SHIFT") == 0) {
    dl.cmd = DuckyLine::SHIFT_KEY;
    dl.payload[0] = '\0';
  } else if (strncmp(line, "REPEAT ", 7) == 0) {
    dl.cmd = DuckyLine::REPEAT;
    dl.value = atoi(line + 7);
  } else {
    dl.cmd = DuckyLine::REM; // Unknown — skip silently
  }

  return dl;
}

// ---------------------------------------------------------------------------
// HID keycode tables
// ---------------------------------------------------------------------------

// Returns HID keycode for a named key ("ENTER", "F5", "a", etc.)
// Returns 0 if not recognized.
uint8_t UsbHidActivity::keyNameToHidCode(const char* name) {
  // Single letter: a-z
  if (name[1] == '\0') {
    char c = name[0];
    if (c >= 'a' && c <= 'z') return static_cast<uint8_t>(0x04 + (c - 'a'));
    if (c >= 'A' && c <= 'Z') return static_cast<uint8_t>(0x04 + (c - 'A'));
    if (c >= '1' && c <= '9') return static_cast<uint8_t>(0x1E + (c - '1'));
    if (c == '0') return 0x27;
  }

  // Named keys
  if (strcmp(name, "ENTER") == 0 || strcmp(name, "RETURN") == 0) return 0x28;
  if (strcmp(name, "ESCAPE") == 0 || strcmp(name, "ESC") == 0) return 0x29;
  if (strcmp(name, "BACKSPACE") == 0 || strcmp(name, "DELETE") == 0) return 0x2A;
  if (strcmp(name, "TAB") == 0)    return 0x2B;
  if (strcmp(name, "SPACE") == 0)  return 0x2C;
  if (strcmp(name, "UP") == 0)     return 0x52;
  if (strcmp(name, "DOWN") == 0)   return 0x51;
  if (strcmp(name, "LEFT") == 0)   return 0x50;
  if (strcmp(name, "RIGHT") == 0)  return 0x4F;

  // F1-F12
  if (name[0] == 'F' && name[1] >= '1' && name[1] <= '9') {
    int n = atoi(name + 1);
    if (n >= 1 && n <= 12) return static_cast<uint8_t>(0x3A + n - 1);
  }

  return 0;
}

// Maps a printable ASCII character to its HID keycode.
// Sets modifier to 0x02 (Left Shift) when needed for uppercase/symbols.
uint8_t UsbHidActivity::charToHidCode(char c, uint8_t& modifier) {
  modifier = 0;

  // Lowercase letters
  if (c >= 'a' && c <= 'z') return static_cast<uint8_t>(0x04 + (c - 'a'));

  // Uppercase letters — need Shift
  if (c >= 'A' && c <= 'Z') {
    modifier = 0x02;
    return static_cast<uint8_t>(0x04 + (c - 'A'));
  }

  // Digits 1-9
  if (c >= '1' && c <= '9') return static_cast<uint8_t>(0x1E + (c - '1'));

  // Control / whitespace
  if (c == '\n' || c == '\r') return 0x28; // Enter
  if (c == '\t')              return 0x2B; // Tab
  if (c == ' ')               return 0x2C; // Space

  // Digit 0 and symbols on the top row (no shift)
  switch (c) {
    case '0': return 0x27;
    case '-': return 0x2D;
    case '=': return 0x2E;
    case '[': return 0x2F;
    case ']': return 0x30;
    case '\\': return 0x31;
    case ';': return 0x33;
    case '\'': return 0x34;
    case '`': return 0x35;
    case ',': return 0x36;
    case '.': return 0x37;
    case '/': return 0x38;
    default: break;
  }

  // Symbols requiring Shift
  modifier = 0x02;
  switch (c) {
    case '!': return 0x1E; // Shift+1
    case '@': return 0x1F; // Shift+2
    case '#': return 0x20; // Shift+3
    case '$': return 0x21; // Shift+4
    case '%': return 0x22; // Shift+5
    case '^': return 0x23; // Shift+6
    case '&': return 0x24; // Shift+7
    case '*': return 0x25; // Shift+8
    case '(': return 0x26; // Shift+9
    case ')': return 0x27; // Shift+0
    case '_': return 0x2D; // Shift+-
    case '+': return 0x2E; // Shift+=
    case '{': return 0x2F; // Shift+[
    case '}': return 0x30; // Shift+]
    case '|': return 0x31; // Shift+backslash
    case ':': return 0x33; // Shift+;
    case '"': return 0x34; // Shift+'
    case '~': return 0x35; // Shift+`
    case '<': return 0x36; // Shift+,
    case '>': return 0x37; // Shift+.
    case '?': return 0x38; // Shift+/
    default: break;
  }

  modifier = 0;
  return 0; // Unknown character — skip
}

// ---------------------------------------------------------------------------
// USB HID stubs (manual TinyUSB implementation required)
// ---------------------------------------------------------------------------

void UsbHidActivity::initUsbHid() {
  // TODO: MANUAL — implement TinyUSB HID descriptor.
  // Switch from CDC-only to CDC+HID composite device.
  // Configure HID report descriptor for keyboard.
  // tud_hid_keyboard_report() will be used for sending keys.
  LOG_INF("UsbHid", "initUsbHid() — TODO: implement TinyUSB HID setup");
}

void UsbHidActivity::sendKeystroke(uint8_t keycode, uint8_t modifier) {
  // TODO: MANUAL — send HID keyboard report.
  // tud_hid_keyboard_report(REPORT_ID, modifier, &keycode);
  // vTaskDelay(10 / portTICK_PERIOD_MS);
  // tud_hid_keyboard_report(REPORT_ID, 0, NULL); // key release
  LOG_INF("UsbHid", "sendKeystroke(0x%02X, 0x%02X) — TODO", keycode, modifier);
  totalKeystrokes++;
}

void UsbHidActivity::sendString(const char* str) {
  // TODO: MANUAL — iterate characters, call sendKeystroke for each.
  for (int i = 0; str[i] != '\0'; i++) {
    uint8_t mod = 0;
    uint8_t code = charToHidCode(str[i], mod);
    if (code) sendKeystroke(code, mod);
  }
}

void UsbHidActivity::releaseAllKeys() {
  // TODO: MANUAL — send empty HID report to release all keys.
  LOG_INF("UsbHid", "releaseAllKeys() — TODO");
}

void UsbHidActivity::deinitUsbHid() {
  // TODO: MANUAL — restore CDC-only USB mode.
  LOG_INF("UsbHid", "deinitUsbHid() — TODO: restore CDC mode");
}

// ---------------------------------------------------------------------------
// Script execution
// ---------------------------------------------------------------------------

void UsbHidActivity::executeNextLine() {
  if (currentLine >= static_cast<int>(script.size())) {
    deinitUsbHid();
    state = DONE;
    requestUpdate();
    return;
  }

  const auto& dl = script[currentLine];

  switch (dl.cmd) {
    case DuckyLine::REM:
      // Comment — skip
      break;

    case DuckyLine::STRING:
      sendString(dl.payload);
      break;

    case DuckyLine::DELAY:
      waiting = true;
      lineStartTime = millis();
      return; // Do NOT advance currentLine yet

    case DuckyLine::ENTER:
      sendKeystroke(0x28, 0);
      break;

    case DuckyLine::TAB:
      sendKeystroke(0x2B, 0);
      break;

    case DuckyLine::SPACE:
      sendKeystroke(0x2C, 0);
      break;

    case DuckyLine::BACKSPACE:
      sendKeystroke(0x2A, 0);
      break;

    case DuckyLine::ESCAPE:
      sendKeystroke(0x29, 0);
      break;

    case DuckyLine::ARROW_UP:
      sendKeystroke(0x52, 0);
      break;

    case DuckyLine::ARROW_DOWN:
      sendKeystroke(0x51, 0);
      break;

    case DuckyLine::ARROW_LEFT:
      sendKeystroke(0x50, 0);
      break;

    case DuckyLine::ARROW_RIGHT:
      sendKeystroke(0x4F, 0);
      break;

    case DuckyLine::F_KEY:
      if (dl.value >= 1 && dl.value <= 12)
        sendKeystroke(static_cast<uint8_t>(0x3A + dl.value - 1), 0);
      break;

    case DuckyLine::GUI_KEY: {
      uint8_t mod = 0x08; // Left GUI modifier
      if (dl.payload[0] != '\0') {
        // Check if payload is a named key (e.g. "ENTER", "r", "TAB")
        uint8_t code = keyNameToHidCode(dl.payload);
        if (!code) {
          uint8_t extra = 0;
          code = charToHidCode(dl.payload[0], extra);
          mod |= extra;
        }
        if (code) sendKeystroke(code, mod);
      } else {
        sendKeystroke(0, mod);
      }
      break;
    }

    case DuckyLine::ALT_KEY: {
      uint8_t mod = 0x04; // Left Alt modifier
      if (dl.payload[0] != '\0') {
        // Support "ALT F4", "ALT TAB", "ALT DELETE", etc.
        uint8_t code = keyNameToHidCode(dl.payload);
        if (!code) {
          uint8_t extra = 0;
          code = charToHidCode(dl.payload[0], extra);
          mod |= extra;
        }
        if (code) sendKeystroke(code, mod);
      } else {
        sendKeystroke(0, mod);
      }
      break;
    }

    case DuckyLine::CTRL_KEY: {
      uint8_t mod = 0x01; // Left Ctrl modifier
      if (dl.payload[0] != '\0') {
        const char* rest = dl.payload;

        // Detect "ALT ..." suffix for CTRL ALT combos (e.g. CTRL ALT DELETE)
        if (strncmp(rest, "ALT", 3) == 0 && (rest[3] == ' ' || rest[3] == '\0')) {
          mod |= 0x04;
          rest += 3;
          while (*rest == ' ') rest++;
        }

        // Detect "SHIFT ..." suffix for CTRL SHIFT combos
        if (strncmp(rest, "SHIFT", 5) == 0 && (rest[5] == ' ' || rest[5] == '\0')) {
          mod |= 0x02;
          rest += 5;
          while (*rest == ' ') rest++;
        }

        if (*rest != '\0') {
          uint8_t code = keyNameToHidCode(rest);
          if (!code) {
            uint8_t extra = 0;
            code = charToHidCode(*rest, extra);
            // Don't mix shift from charToHidCode into mod for CTRL combos — the
            // payload is typically a bare letter, not a shifted symbol.
          }
          if (code) sendKeystroke(code, mod);
        } else {
          sendKeystroke(0, mod);
        }
      } else {
        sendKeystroke(0, mod);
      }
      break;
    }

    case DuckyLine::SHIFT_KEY: {
      uint8_t mod = 0x02; // Left Shift modifier
      if (dl.payload[0] != '\0') {
        uint8_t code = keyNameToHidCode(dl.payload);
        if (!code) {
          uint8_t extra = 0;
          code = charToHidCode(dl.payload[0], extra);
          // extra shift already implied by mod
        }
        if (code) sendKeystroke(code, mod);
      } else {
        sendKeystroke(0, mod);
      }
      break;
    }

    case DuckyLine::REPEAT: {
      if (currentLine > 0 && dl.value > 0) {
        // Find the previous non-REM, non-REPEAT line
        int prevLine = currentLine - 1;
        while (prevLine >= 0 && (script[prevLine].cmd == DuckyLine::REM ||
                                  script[prevLine].cmd == DuckyLine::REPEAT)) {
          prevLine--;
        }
        if (prevLine >= 0) {
          int savedCurrent = currentLine;
          for (int r = 0; r < dl.value; r++) {
            currentLine = prevLine;
            executeNextLine();
            // executeNextLine() increments currentLine at the end; restore
            currentLine = savedCurrent;
          }
        }
      }
      break;
    }

    case DuckyLine::KEY:
    case DuckyLine::COMBO:
      // Not generated by parseLine — handled as REM (skip)
      break;
  }

  currentLine++;
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void UsbHidActivity::loop() {
  if (state == FILE_SELECT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    const int count = static_cast<int>(duckyFiles.size());

    if (count > 0) {
      buttonNavigator.onNext([this, count] {
        fileIndex = ButtonNavigator::nextIndex(fileIndex, count);
        requestUpdate();
      });
      buttonNavigator.onPrevious([this, count] {
        fileIndex = ButtonNavigator::previousIndex(fileIndex, count);
        requestUpdate();
      });

      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        if (fileIndex < count) {
          if (parseScript(duckyFiles[fileIndex].name)) {
            previewScroll = 0;
            state = PREVIEW;
          } else {
            state = ERROR;
          }
          requestUpdate();
        }
      }
    }
    return;
  }

  if (state == PREVIEW) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = FILE_SELECT;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (previewScroll > 0) { previewScroll--; requestUpdate(); }
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      int maxScroll = static_cast<int>(previewLines.size()) - 1;
      if (maxScroll < 0) maxScroll = 0;
      if (previewScroll < maxScroll) { previewScroll++; requestUpdate(); }
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      state = CONFIRM_RUN;
      requestUpdate();
    }
    return;
  }

  if (state == CONFIRM_RUN) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = PREVIEW;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      initUsbHid();
      totalKeystrokes = 0;
      currentLine = 0;
      waiting = false;
      runStartTime = millis();
      state = RUNNING;
      requestUpdate();
    }
    return;
  }

  if (state == RUNNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      releaseAllKeys();
      deinitUsbHid();
      state = FILE_SELECT;
      requestUpdate();
      return;
    }

    if (waiting) {
      if (millis() - lineStartTime >= static_cast<unsigned long>(script[currentLine].value)) {
        waiting = false;
        currentLine++;
        requestUpdate();
      }
      return; // Still waiting on DELAY
    }

    executeNextLine();
    return;
  }

  if (state == DONE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = FILE_SELECT;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Re-run: re-parse (previewLines already populated) and restart
      if (!duckyFiles.empty() && fileIndex < static_cast<int>(duckyFiles.size())) {
        parseScript(duckyFiles[fileIndex].name);
      }
      totalKeystrokes = 0;
      currentLine = 0;
      waiting = false;
      runStartTime = millis();
      initUsbHid();
      state = RUNNING;
      requestUpdate();
    }
    return;
  }

  if (state == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = FILE_SELECT;
      requestUpdate();
    }
    return;
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void UsbHidActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const int headerY    = metrics.topPadding;
  const int headerBot  = headerY + metrics.headerHeight;
  const int listTop    = headerBot + metrics.verticalSpacing;
  const int hintsTop   = pageHeight - metrics.buttonHintsHeight;
  const int listHeight = hintsTop - metrics.verticalSpacing - listTop;

  // -------------------------------------------------------------------------
  if (state == FILE_SELECT) {
    GUI.drawHeader(renderer, Rect{0, headerY, pageWidth, metrics.headerHeight}, "USB HID");

    const int count = static_cast<int>(duckyFiles.size());
    if (count == 0) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, "No scripts found");
      renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 5,
                                "Place .txt files in /biscuit/ducky/");
      const auto labels = mappedInput.mapLabels("Back", "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer, Rect{0, listTop, pageWidth, listHeight}, count, fileIndex,
          [this](int i) -> std::string { return duckyFiles[i].name; },
          [](int) -> std::string { return "DuckyScript payload"; },
          [](int) -> UIIcon { return UIIcon::File; },
          [](int) -> std::string { return ""; });

      const auto labels = mappedInput.mapLabels("Back", "Load", "^", "v");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    renderer.displayBuffer();
    return;
  }

  // -------------------------------------------------------------------------
  if (state == PREVIEW) {
    // Show filename as header subtitle
    const char* fname = (!duckyFiles.empty() && fileIndex < static_cast<int>(duckyFiles.size()))
                        ? duckyFiles[fileIndex].name : "Script";

    char subtitle[32];
    snprintf(subtitle, sizeof(subtitle), "%d cmds", static_cast<int>(script.size()));
    GUI.drawHeader(renderer, Rect{0, headerY, pageWidth, metrics.headerHeight}, fname, subtitle);

    // Draw preview lines
    const int lineH = 22;
    int y = listTop;
    int shown = 0;
    for (int i = previewScroll; i < static_cast<int>(previewLines.size()) && y + lineH <= hintsTop; i++) {
      renderer.drawText(SMALL_FONT_ID, 8, y, previewLines[i].c_str());
      y += lineH;
      shown++;
    }

    // Estimate runtime: 50ms per non-delay command + actual delay values
    int estimatedMs = 0;
    for (const auto& dl : script) {
      if (dl.cmd == DuckyLine::DELAY) {
        estimatedMs += dl.value;
      } else if (dl.cmd != DuckyLine::REM) {
        estimatedMs += 50;
      }
    }
    char statBuf[48];
    snprintf(statBuf, sizeof(statBuf), "~%d commands, est. %ds",
             static_cast<int>(script.size()), estimatedMs / 1000);
    renderer.drawCenteredText(SMALL_FONT_ID, hintsTop - 20, statBuf);

    const auto labels = mappedInput.mapLabels("Back", "Continue", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // -------------------------------------------------------------------------
  if (state == CONFIRM_RUN) {
    GUI.drawHeader(renderer, Rect{0, headerY, pageWidth, metrics.headerHeight}, "Ready to Run");

    int y = pageHeight / 2 - 70;
    renderer.drawCenteredText(UI_10_FONT_ID, y, "This will type keystrokes");
    y += 30;
    renderer.drawCenteredText(UI_10_FONT_ID, y, "on the connected PC.");
    y += 40;
    renderer.drawCenteredText(SMALL_FONT_ID, y, "Connect USB-C and focus a text editor.");
    y += 24;
    renderer.drawCenteredText(SMALL_FONT_ID, y, "Continue?");

    const auto labels = mappedInput.mapLabels("Back", "Run!", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // -------------------------------------------------------------------------
  if (state == RUNNING) {
    GUI.drawHeader(renderer, Rect{0, headerY, pageWidth, metrics.headerHeight}, "Running...");

    const int total = static_cast<int>(script.size());
    const int done  = currentLine;

    // Current command text
    char cmdBuf[80] = "—";
    if (currentLine < total) {
      const auto& dl = script[currentLine];
      switch (dl.cmd) {
        case DuckyLine::STRING:
          snprintf(cmdBuf, sizeof(cmdBuf), "STRING: %.60s", dl.payload);
          break;
        case DuckyLine::DELAY:
          snprintf(cmdBuf, sizeof(cmdBuf), "DELAY %d ms", dl.value);
          break;
        case DuckyLine::ENTER:     strncpy(cmdBuf, "ENTER",     sizeof(cmdBuf) - 1); break;
        case DuckyLine::TAB:       strncpy(cmdBuf, "TAB",       sizeof(cmdBuf) - 1); break;
        case DuckyLine::SPACE:     strncpy(cmdBuf, "SPACE",     sizeof(cmdBuf) - 1); break;
        case DuckyLine::BACKSPACE: strncpy(cmdBuf, "BACKSPACE", sizeof(cmdBuf) - 1); break;
        case DuckyLine::ESCAPE:    strncpy(cmdBuf, "ESCAPE",    sizeof(cmdBuf) - 1); break;
        case DuckyLine::ARROW_UP:  strncpy(cmdBuf, "UP",        sizeof(cmdBuf) - 1); break;
        case DuckyLine::ARROW_DOWN: strncpy(cmdBuf, "DOWN",     sizeof(cmdBuf) - 1); break;
        case DuckyLine::ARROW_LEFT: strncpy(cmdBuf, "LEFT",     sizeof(cmdBuf) - 1); break;
        case DuckyLine::ARROW_RIGHT: strncpy(cmdBuf, "RIGHT",   sizeof(cmdBuf) - 1); break;
        case DuckyLine::GUI_KEY:
          snprintf(cmdBuf, sizeof(cmdBuf), "GUI %s", dl.payload);
          break;
        case DuckyLine::ALT_KEY:
          snprintf(cmdBuf, sizeof(cmdBuf), "ALT %s", dl.payload);
          break;
        case DuckyLine::CTRL_KEY:
          snprintf(cmdBuf, sizeof(cmdBuf), "CTRL %s", dl.payload);
          break;
        case DuckyLine::SHIFT_KEY:
          snprintf(cmdBuf, sizeof(cmdBuf), "SHIFT %s", dl.payload);
          break;
        case DuckyLine::F_KEY:
          snprintf(cmdBuf, sizeof(cmdBuf), "F%d", dl.value);
          break;
        case DuckyLine::REPEAT:
          snprintf(cmdBuf, sizeof(cmdBuf), "REPEAT x%d", dl.value);
          break;
        case DuckyLine::REM:
          strncpy(cmdBuf, "REM (comment)", sizeof(cmdBuf) - 1);
          break;
        default:
          strncpy(cmdBuf, "...", sizeof(cmdBuf) - 1);
          break;
      }
    }

    // Line counter
    char lineBuf[32];
    snprintf(lineBuf, sizeof(lineBuf), "Line %d / %d", done + 1, total);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 60, lineBuf, true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 - 25, cmdBuf);

    // Progress bar
    if (total > 0) {
      const int barW = pageWidth - 60;
      const int barH = 14;
      const int barX = 30;
      const int barY = pageHeight / 2 + 10;
      renderer.drawRect(barX, barY, barW, barH, true);
      int fill = (done * barW) / total;
      if (fill > 0) renderer.fillRect(barX, barY, fill, barH, true);
    }

    // Keystroke counter
    char ksBuf[32];
    snprintf(ksBuf, sizeof(ksBuf), "%d keystrokes sent", totalKeystrokes);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 35, ksBuf);

    const auto labels = mappedInput.mapLabels("Abort", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // -------------------------------------------------------------------------
  if (state == DONE) {
    GUI.drawHeader(renderer, Rect{0, headerY, pageWidth, metrics.headerHeight}, "Done");

    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 50,
                              "Script complete.", true, EpdFontFamily::BOLD);

    unsigned long elapsed = millis() - runStartTime;
    char statBuf[64];
    snprintf(statBuf, sizeof(statBuf), "%d keystrokes in %lu.%lus",
             totalKeystrokes, elapsed / 1000, (elapsed % 1000) / 100);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, statBuf);

    const auto labels = mappedInput.mapLabels("Back", "Re-run", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // -------------------------------------------------------------------------
  if (state == ERROR) {
    GUI.drawHeader(renderer, Rect{0, headerY, pageWidth, metrics.headerHeight}, "Error");

    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, errorMsg);

    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  renderer.displayBuffer(); // Fallback
}
