#include "TodoActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

static constexpr const char* TODO_PATH = "/todo/list.json";
static constexpr int MAX_ITEMS = 16;
static constexpr unsigned long LONG_PRESS_MS = 800;

TodoActivity::TodoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Todo", renderer, mappedInput) {}

void TodoActivity::loadItems() {
  items.clear();

  String raw = Storage.readFile(TODO_PATH);
  if (raw.isEmpty()) {
    LOG_DBG("TODO", "No todo file found at %s", TODO_PATH);
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) {
    LOG_ERR("TODO", "Failed to parse %s", TODO_PATH);
    return;
  }

  JsonArray arr = doc["items"].as<JsonArray>();
  if (!arr) return;

  items.reserve(std::min((int)arr.size(), MAX_ITEMS));
  for (JsonObject obj : arr) {
    if ((int)items.size() >= MAX_ITEMS) break;
    Item item{};
    const char* text = obj["text"] | "";
    strncpy(item.text, text, sizeof(item.text) - 1);
    item.text[sizeof(item.text) - 1] = '\0';
    item.done = obj["done"] | false;
    items.push_back(item);
  }

  LOG_DBG("TODO", "Loaded %zu items", items.size());
}

void TodoActivity::saveItems() const {
  JsonDocument doc;
  JsonArray arr = doc["items"].to<JsonArray>();
  for (const auto& item : items) {
    JsonObject obj = arr.add<JsonObject>();
    obj["text"] = item.text;
    obj["done"] = item.done;
  }

  String jsonStr;
  serializeJson(doc, jsonStr);

  Storage.mkdir("/todo", true);
  if (!Storage.writeFile(TODO_PATH, jsonStr)) {
    LOG_ERR("TODO", "Failed to write %s", TODO_PATH);
  }
}

void TodoActivity::onEnter() {
  Activity::onEnter();
  loadItems();
  selectedIndex = 0;
  requestUpdate();
}

void TodoActivity::onExit() {
  items.clear();
  Activity::onExit();
}

void TodoActivity::loop() {
  const int count = static_cast<int>(items.size());
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

  // Long-press Confirm: toggle done/undone
  if (!longPressFired && !items.empty() && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    items[selectedIndex].done = !items[selectedIndex].done;
    saveItems();
    requestUpdate();
    return;
  }
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    longPressFired = false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  if (count == 0) return;

  buttonNavigator.onNextRelease([this, count] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, count);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, count] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, count);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, count, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, count, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, count, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, count, pageItems);
    requestUpdate();
  });
}

void TodoActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Count remaining
  int remaining = 0;
  for (const auto& item : items) {
    if (!item.done) remaining++;
  }

  // Header with remaining count
  char headerBuf[48];
  if (items.empty()) {
    snprintf(headerBuf, sizeof(headerBuf), "%s", tr(STR_TODO_LIST));
  } else {
    snprintf(headerBuf, sizeof(headerBuf), "%s  (%d %s)", tr(STR_TODO_LIST), remaining, tr(STR_TODO_REMAINING));
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerBuf);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (items.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_TODO_EMPTY));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, (int)items.size(), selectedIndex,
        [this](int index) -> std::string {
          char buf[72];
          const char* checkbox = items[index].done ? "[x] " : "[ ] ";
          snprintf(buf, sizeof(buf), "%s%s", checkbox, items[index].text);
          return std::string(buf);
        },
        nullptr, nullptr,
        nullptr,
        false,
        [this](int index) -> bool { return items[index].done; });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
