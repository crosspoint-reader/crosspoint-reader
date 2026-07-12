#include "EventLoggerActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <cstdlib>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void EventLoggerActivity::formatTimestamp(uint64_t ms, char* buf, int len) {
  uint64_t sec = ms / 1000;
  unsigned int h = (unsigned int)((sec / 3600) % 24);
  unsigned int m = (unsigned int)((sec / 60) % 60);
  unsigned int s = (unsigned int)(sec % 60);
  snprintf(buf, len, "%02u:%02u:%02u", h, m, s);
}

void EventLoggerActivity::loadEntries() {
  entries.clear();
  auto file = Storage.open(LOG_PATH);
  if (!file) return;

  char line[256];
  // Collect all lines (newest at end), keep last MAX_ENTRIES
  std::vector<std::string> lines;
  while (file.available()) {
    int len = 0;
    while (file.available() && len < (int)sizeof(line) - 1) {
      char c = (char)file.read();
      if (c == '\n') break;
      line[len++] = c;
    }
    line[len] = '\0';
    if (len > 0) lines.push_back(std::string(line));
    if (lines.size() > MAX_ENTRIES) lines.erase(lines.begin());
  }
  file.close();

  // Parse newest-first
  for (int i = (int)lines.size() - 1; i >= 0; i--) {
    const char* s = lines[i].c_str();
    const char* comma = strchr(s, ',');
    if (!comma) continue;
    LogEntry e;
    e.timestampMs = (uint64_t)strtoull(s, nullptr, 10);
    strncpy(e.text, comma + 1, sizeof(e.text) - 1);
    e.text[sizeof(e.text) - 1] = '\0';
    entries.push_back(e);
  }
}

void EventLoggerActivity::saveEntry(const char* text) {
  Storage.mkdir("/biscuit");
  Storage.mkdir("/biscuit/logs");
  auto file = Storage.open(LOG_PATH, O_WRITE | O_CREAT | O_APPEND);
  if (!file) return;
  char line[256];
  snprintf(line, sizeof(line), "%llu,%s\n", (unsigned long long)millis(), text);
  file.print(line);
  file.close();
}

void EventLoggerActivity::onEnter() {
  Activity::onEnter();
  loadEntries();
  state = LOG_LIST;
  selectedIndex = 0;
  requestUpdate();
}

void EventLoggerActivity::onExit() { Activity::onExit(); }

void EventLoggerActivity::loop() {
  if (state == LOG_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { finish(); return; }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, (int)entries.size());
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, (int)entries.size());
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && !entries.empty()) {
      state = VIEW_ENTRY;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      state = COMPOSE;
      startActivityForResult(
          std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "New Note", "", 127),
          [this](const ActivityResult& r) {
            state = LOG_LIST;
            if (!r.isCancelled) {
              const auto& text = std::get<KeyboardResult>(r.data).text;
              saveEntry(text.c_str());
              loadEntries();
              selectedIndex = 0;
            }
          });
    }
    return;
  }

  if (state == VIEW_ENTRY) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = LOG_LIST;
      requestUpdate();
    }
    return;
  }
}

void EventLoggerActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Event Logger");

  if (state == LOG_LIST) renderList();
  else if (state == VIEW_ENTRY) renderView();

  renderer.displayBuffer();
}

void EventLoggerActivity::renderList() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listH = pageHeight - listTop - metrics.buttonHintsHeight;

  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, listTop + listH / 2, "No entries. Right=Compose");
  } else {
    GUI.drawList(renderer, Rect{0, listTop, pageWidth, listH}, (int)entries.size(), selectedIndex,
      [this](int i) -> std::string { return entries[i].text; },
      [this](int i) -> std::string {
        char buf[16];
        formatTimestamp(entries[i].timestampMs, buf, sizeof(buf));
        return buf;
      });
  }

  const auto labels = mappedInput.mapLabels("Back", "View", "", "New");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void EventLoggerActivity::renderView() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int top = metrics.topPadding + metrics.headerHeight + 10;

  if (selectedIndex < 0 || selectedIndex >= (int)entries.size()) return;
  const LogEntry& e = entries[selectedIndex];

  char tsBuf[16];
  formatTimestamp(e.timestampMs, tsBuf, sizeof(tsBuf));
  renderer.drawCenteredText(SMALL_FONT_ID, top, tsBuf);
  renderer.drawCenteredText(UI_10_FONT_ID, top + 30, e.text);

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
