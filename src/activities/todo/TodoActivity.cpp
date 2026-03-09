#include "TodoActivity.h"

#include <Arduino.h>
#include <HalStorage.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ScreenComponents.h"
#include "SpiBusMutex.h"
#include "activities/TaskShutdown.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "fontIds.h"

namespace {
constexpr int HEADER_HEIGHT = 50;
constexpr int ITEM_HEIGHT = 35;
constexpr int MARGIN_X = 10;
constexpr int CHECKBOX_SIZE = 20;
constexpr unsigned long LONG_CONFIRM_MS = 600;
constexpr size_t TODO_ENTRY_MAX_TEXT_LENGTH = 300;

bool isValidItemIndex(const int index, const size_t count) { return index >= 0 && static_cast<size_t>(index) < count; }

std::string trimEntryText(const std::string& text) {
  size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
    start++;
  }

  size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    end--;
  }

  std::string trimmed = text.substr(start, end - start);
  if (trimmed.size() > TODO_ENTRY_MAX_TEXT_LENGTH) {
    trimmed.resize(TODO_ENTRY_MAX_TEXT_LENGTH);
  }
  return trimmed;
}
}  // namespace

TodoActivity::TodoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath,
                           std::string dateTitle, void* onBackCtx, void (*onBack)(void*))
    : ActivityWithSubactivity("Todo", renderer, mappedInput),
      filePath(std::move(filePath)),
      dateTitle(std::move(dateTitle)),
      onBackCtx(onBackCtx),
      onBack(onBack) {}

void TodoActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  skipInitialInput = true;
  loadTasks();
  requestUpdate();
}

void TodoActivity::onExit() { ActivityWithSubactivity::onExit(); }

void TodoActivity::loop() {
  ActivityWithSubactivity::loop();
  if (subActivity) {
    return;
  }

  if (skipInitialInput) {
    const bool clear = !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                       !mappedInput.wasReleased(MappedInputManager::Button::Confirm) &&
                       !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                       !mappedInput.wasReleased(MappedInputManager::Button::Back);
    if (clear) {
      skipInitialInput = false;
    }
    return;
  }

  // Handle Back
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (onBack != nullptr) onBack(onBackCtx);
    return;
  }

  // Capture input state
  const bool upPressed = mappedInput.wasPressed(MappedInputManager::Button::Up);
  const bool downPressed = mappedInput.wasPressed(MappedInputManager::Button::Down);
  const bool leftPressed = mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool rightPressed = mappedInput.wasPressed(MappedInputManager::Button::Right);
  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  const bool longConfirm = confirmReleased && mappedInput.getHeldTime() >= LONG_CONFIRM_MS;

  const int taskCount = static_cast<int>(items.size());
  // +1 for the "Add New Task" button at the end
  const int totalItems = taskCount + 1;
  const int visibleItems = (renderer.getScreenHeight() - HEADER_HEIGHT) / ITEM_HEIGHT;

  // Navigation (Up/Down only)
  if (upPressed) {
    if (selectedIndex > 0) {
      selectedIndex--;
      // Adjust scroll if moving above view
      if (selectedIndex < scrollOffset) {
        scrollOffset = selectedIndex;
      }
      requestUpdate();
    }
  } else if (downPressed) {
    if (selectedIndex < totalItems - 1) {
      selectedIndex++;
      // Adjust scroll if moving below view
      if (selectedIndex >= scrollOffset + visibleItems) {
        scrollOffset = selectedIndex - visibleItems + 1;
      }
      requestUpdate();
    }
  }

  // Reordering (Left/Right) - only for task items, not headers or "Add New"
  if (isValidItemIndex(selectedIndex, items.size()) && !items[static_cast<size_t>(selectedIndex)].isHeader) {
    if (leftPressed) {
      // Move task UP in list (swap with previous item, skipping headers)
      int targetIndex = selectedIndex - 1;
      while (targetIndex >= 0 && items[static_cast<size_t>(targetIndex)].isHeader) {
        targetIndex--;
      }
      if (targetIndex >= 0) {
        std::swap(items[static_cast<size_t>(selectedIndex)], items[static_cast<size_t>(targetIndex)]);
        selectedIndex = targetIndex;
        if (selectedIndex < scrollOffset) {
          scrollOffset = selectedIndex;
        }
        saveTasks();
        requestUpdate();
      }
    } else if (rightPressed) {
      // Move task DOWN in list (swap with next item, skipping headers)
      int targetIndex = selectedIndex + 1;
      while (targetIndex < taskCount && items[static_cast<size_t>(targetIndex)].isHeader) {
        targetIndex++;
      }
      if (targetIndex < taskCount) {
        std::swap(items[static_cast<size_t>(selectedIndex)], items[static_cast<size_t>(targetIndex)]);
        selectedIndex = targetIndex;
        if (selectedIndex >= scrollOffset + visibleItems) {
          scrollOffset = selectedIndex - visibleItems + 1;
        }
        saveTasks();
        requestUpdate();
      }
    }
  }

  // Toggle / Select
  if (confirmReleased) {
    if (isValidItemIndex(selectedIndex, items.size())) {
      const bool isHeader = items[static_cast<size_t>(selectedIndex)].isHeader;
      if (isHeader || longConfirm) {
        editCurrentEntry();
      } else {
        toggleCurrentTask();
      }
    } else {
      addNewEntry(longConfirm);
    }
  }
}

void TodoActivity::loadTasks() {
  SpiBusMutex::Guard guard;
  items.clear();

  if (!Storage.exists(filePath.c_str())) {
    return;
  }

  FsFile file = Storage.open(filePath.c_str(), O_RDONLY);
  if (!file) {
    return;
  }

  char buffer[256];
  std::string line;
  while (file.available()) {
    int n = file.read(buffer, sizeof(buffer));
    if (n <= 0) break;

    for (int i = 0; i < n; i++) {
      if (buffer[i] == '\n') {
        processTaskLine(line);
        line.clear();
      } else {
        line.push_back(buffer[i]);
      }
    }
  }
  if (!line.empty()) {
    processTaskLine(line);
  }
  file.close();
}

void TodoActivity::processTaskLine(std::string& line) {
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }

  TodoItem item;
  if (line.rfind("- [ ] ", 0) == 0) {
    item.checked = false;
    item.isHeader = false;
    item.text = line.substr(6);
  } else if (line.rfind("- [x] ", 0) == 0 || line.rfind("- [X] ", 0) == 0) {
    item.checked = true;
    item.isHeader = false;
    item.text = line.substr(6);
  } else if (line.rfind("> ", 0) == 0) {
    // Markdown blockquote — agenda entry written when markdown is enabled.
    item.checked = false;
    item.isHeader = true;
    item.text = line.substr(2);
  } else {
    item.checked = false;
    item.isHeader = true;
    item.text = line;
  }
  items.push_back(item);
}

void TodoActivity::saveTasks() {
  SpiBusMutex::Guard guard;

  // Ensure directory exists
  const auto slashPos = filePath.find_last_of('/');
  if (slashPos != std::string::npos && slashPos > 0) {
    std::string dirPath = filePath.substr(0, slashPos);
    Storage.mkdir(dirPath.c_str());
  }

  const std::string tempPath = filePath + ".tmp";
  const std::string backupPath = filePath + ".bak";

  if (Storage.exists(tempPath.c_str())) {
    Storage.remove(tempPath.c_str());
  }

  FsFile file;
  if (!Storage.openFileForWrite("TDO", tempPath.c_str(), file)) {
    return;
  }

  const bool markdownFile = filePath.size() >= 3 && filePath.compare(filePath.size() - 3, 3, ".md") == 0;
  bool writeFailed = false;
  for (const auto& item : items) {
    if (item.isHeader) {
      if (markdownFile && file.print("> ") == 0) {
        writeFailed = true;
        break;
      }
      if (file.println(item.text.c_str()) == 0) {
        writeFailed = true;
        break;
      }
    } else {
      if (file.print("- [") == 0 || file.print(item.checked ? "x" : " ") == 0 || file.print("] ") == 0 ||
          file.println(item.text.c_str()) == 0) {
        writeFailed = true;
        break;
      }
    }
  }
  file.close();
  if (writeFailed) {
    Storage.remove(tempPath.c_str());
    return;
  }

  const bool hasExisting = Storage.exists(filePath.c_str());
  if (Storage.exists(backupPath.c_str())) {
    Storage.remove(backupPath.c_str());
  }

  if (hasExisting && !Storage.rename(filePath.c_str(), backupPath.c_str())) {
    Storage.remove(tempPath.c_str());
    return;
  }

  if (!Storage.rename(tempPath.c_str(), filePath.c_str())) {
    if (hasExisting) {
      Storage.rename(backupPath.c_str(), filePath.c_str());
    }
    Storage.remove(tempPath.c_str());
    return;
  }

  if (hasExisting) {
    Storage.remove(backupPath.c_str());
  }
}

void TodoActivity::toggleCurrentTask() {
  if (isValidItemIndex(selectedIndex, items.size())) {
    auto& item = items[static_cast<size_t>(selectedIndex)];
    if (!item.isHeader) {
      item.checked = !item.checked;
      saveTasks();
      requestUpdate();
    }
  }
}

void TodoActivity::addNewEntry(const bool agendaEntry) {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, agendaEntry ? "New Agenda Entry" : "New Task", "",
                                              TODO_ENTRY_MAX_TEXT_LENGTH, false, 10),
      [this, agendaEntry](const ActivityResult& result) {
        if (result.isCancelled) {
          return;
        }

        const std::string trimmedText = trimEntryText(std::get<KeyboardResult>(result.data).text);
        if (!trimmedText.empty()) {
          TodoItem newItem;
          newItem.text = trimmedText;
          newItem.checked = false;
          newItem.isHeader = agendaEntry;
          items.push_back(newItem);
          saveTasks();
          selectedIndex = static_cast<int>(items.size()) - 1;
          const int visibleItems = (renderer.getScreenHeight() - HEADER_HEIGHT) / ITEM_HEIGHT;
          if (selectedIndex >= scrollOffset + visibleItems) {
            scrollOffset = selectedIndex - visibleItems + 1;
          }
        }
      });
}

void TodoActivity::editCurrentEntry() {
  if (!isValidItemIndex(selectedIndex, items.size())) {
    return;
  }

  const size_t editIndex = static_cast<size_t>(selectedIndex);
  const bool isHeader = items[editIndex].isHeader;
  const char* title = isHeader ? "Edit Agenda Entry" : "Edit Task";
  const std::string initialText = items[editIndex].text;

  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, title, initialText,
                                                                 TODO_ENTRY_MAX_TEXT_LENGTH, false, 10),
                         [this, editIndex](const ActivityResult& result) {
                           if (result.isCancelled) {
                             return;
                           }

                           if (!isValidItemIndex(static_cast<int>(editIndex), items.size())) {
                             return;
                           }

                           const std::string trimmedText = trimEntryText(std::get<KeyboardResult>(result.data).text);
                           if (trimmedText.empty()) {
                             const auto removeAt = static_cast<std::vector<TodoItem>::difference_type>(editIndex);
                             items.erase(items.begin() + removeAt);
                             selectedIndex = std::min(selectedIndex, static_cast<int>(items.size()));
                           } else {
                             items[editIndex].text = trimmedText;
                           }

                           const int visibleItems = (renderer.getScreenHeight() - HEADER_HEIGHT) / ITEM_HEIGHT;
                           if (selectedIndex < scrollOffset) {
                             scrollOffset = selectedIndex;
                           } else if (selectedIndex >= scrollOffset + visibleItems) {
                             scrollOffset = selectedIndex - visibleItems + 1;
                           }

                           saveTasks();
                         });
}

void TodoActivity::render(Activity::RenderLock&& lock) { renderScreen(); }

void TodoActivity::renderScreen() {
  renderer.clearScreen();

  // Header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, dateTitle.c_str(), true, EpdFontFamily::BOLD);
  renderer.drawLine(0, HEADER_HEIGHT, renderer.getScreenWidth(), HEADER_HEIGHT);

  const int visibleItems = (renderer.getScreenHeight() - HEADER_HEIGHT) / ITEM_HEIGHT;
  const int totalItems = static_cast<int>(items.size()) + 1;  // +1 for "Add New"

  for (int i = 0; i < visibleItems; i++) {
    int itemIndex = scrollOffset + i;
    if (itemIndex >= totalItems) break;

    int y = HEADER_HEIGHT + i * ITEM_HEIGHT;
    bool isSelected = (itemIndex == selectedIndex);

    if (isSelected) {
      renderer.fillRect(0, y, renderer.getScreenWidth(), ITEM_HEIGHT);
    }

    if (isValidItemIndex(itemIndex, items.size())) {
      renderItem(y, items[static_cast<size_t>(itemIndex)], isSelected);
    } else {
      // "Add New" button (short Confirm = TODO, long Confirm = agenda/note).
      renderer.drawCenteredText(UI_10_FONT_ID, y + 5, "+ Add New Entry", !isSelected);
    }
  }

  // Hints - show reorder hints for task items
  const bool hasSelection = isValidItemIndex(selectedIndex, items.size());
  const bool canReorder = hasSelection && !items[static_cast<size_t>(selectedIndex)].isHeader;
  const bool addNewSelected = !hasSelection;
  const char* confirmLabel = addNewSelected ? "Add TODO" : (canReorder ? "Toggle" : "Edit");
  const char* leftLabel = canReorder ? "Move Up" : (addNewSelected ? "Hold OK:Agenda" : "Hold OK:Edit");
  const auto labels = mappedInput.mapLabels("Back", confirmLabel, leftLabel, canReorder ? "Move Dn" : "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void TodoActivity::renderItem(int y, const TodoItem& item, bool isSelected) const {
  int x = MARGIN_X;

  if (!item.isHeader) {
    // Draw Checkbox
    if (isSelected) {
      // Inverted colors for selection: White border on Black background
      renderer.drawRect(x, y + 8, CHECKBOX_SIZE, CHECKBOX_SIZE, false);  // Clear rect inside black fill
      if (item.checked) {
        // Draw X or checkmark
        renderer.drawLine(x + 4, y + 12, x + 16, y + 24, false);
        renderer.drawLine(x + 16, y + 12, x + 4, y + 24, false);
      }
    } else {
      // Standard colors: Black border on White background
      renderer.drawRect(x, y + 8, CHECKBOX_SIZE, CHECKBOX_SIZE);
      if (item.checked) {
        renderer.drawLine(x + 4, y + 12, x + 16, y + 24);
        renderer.drawLine(x + 16, y + 12, x + 4, y + 24);
      }
    }
    x += CHECKBOX_SIZE + 10;
  }

  // Draw Text
  const int textY = y + (ITEM_HEIGHT - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  const int maxTextWidth = renderer.getScreenWidth() - x - MARGIN_X;

  // Truncate text if needed
  std::string text = renderer.truncatedText(UI_10_FONT_ID, item.text.c_str(), maxTextWidth);

  renderer.drawText(UI_10_FONT_ID, x, textY, text.c_str(), !isSelected);

  if (item.checked && !item.isHeader) {
    int textWidth = renderer.getTextWidth(UI_10_FONT_ID, text.c_str());
    int lineY = textY + renderer.getLineHeight(UI_10_FONT_ID) / 2;
    renderer.drawLine(x, lineY, x + textWidth, lineY, !isSelected);
  }
}
