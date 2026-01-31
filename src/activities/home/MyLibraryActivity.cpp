#include "MyLibraryActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "ScreenComponents.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
// Layout constants
constexpr int TAB_BAR_Y = 15;
constexpr int CONTENT_START_Y = 60;
constexpr int LINE_HEIGHT = 30;
constexpr int LEFT_MARGIN = 20;
constexpr int RIGHT_MARGIN = 40;  // Extra space for scroll indicator

// Timing thresholds for button behavior
constexpr int LONG_PRESS_MS = 450;          // Long press: change tab
constexpr int DOUBLE_PRESS_MS = 120;        // Double press: skip page
constexpr unsigned long GO_HOME_MS = 1000;  // Long press back: go to root

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    if (str1.back() == '/' && str2.back() != '/') return true;
    if (str1.back() != '/' && str2.back() == '/') return false;
    return lexicographical_compare(
        begin(str1), end(str1), begin(str2), end(str2),
        [](const char& char1, const char& char2) { return tolower(char1) < tolower(char2); });
  });
}
}  // namespace

int MyLibraryActivity::getPageItems() const {
  const int screenHeight = renderer.getScreenHeight();
  const int bottomBarHeight = 60;  // Space for button hints
  const int availableHeight = screenHeight - CONTENT_START_Y - bottomBarHeight;
  int items = availableHeight / LINE_HEIGHT;
  if (items < 1) {
    items = 1;
  }
  return items;
}

int MyLibraryActivity::getCurrentItemCount() const {
  if (currentTab == Tab::Recent) {
    return static_cast<int>(bookTitles.size());
  }
  return static_cast<int>(files.size());
}

int MyLibraryActivity::getTotalPages() const {
  const int itemCount = getCurrentItemCount();
  const int pageItems = getPageItems();
  if (itemCount == 0) return 1;
  return (itemCount + pageItems - 1) / pageItems;
}

int MyLibraryActivity::getCurrentPage() const {
  const int pageItems = getPageItems();
  return selectorIndex / pageItems + 1;
}

void MyLibraryActivity::loadRecentBooks() {
  constexpr size_t MAX_RECENT_BOOKS = 20;

  bookTitles.clear();
  bookPaths.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  bookTitles.reserve(std::min(books.size(), MAX_RECENT_BOOKS));
  bookPaths.reserve(std::min(books.size(), MAX_RECENT_BOOKS));

  for (const auto& path : books) {
    // Limit to maximum number of recent books
    if (bookTitles.size() >= MAX_RECENT_BOOKS) {
      break;
    }

    // Skip if file no longer exists
    if (!SdMan.exists(path.c_str())) {
      continue;
    }

    // Extract filename from path for display
    std::string title = path;
    const size_t lastSlash = title.find_last_of('/');
    if (lastSlash != std::string::npos) {
      title = title.substr(lastSlash + 1);
    }

    bookTitles.push_back(title);
    bookPaths.push_back(path);
  }
}

void MyLibraryActivity::loadFiles() {
  files.clear();

  auto root = SdMan.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/");
    } else {
      auto filename = std::string(name);
      if (StringUtils::checkFileExtension(filename, ".epub") || StringUtils::checkFileExtension(filename, ".xtch") ||
          StringUtils::checkFileExtension(filename, ".xtc") || StringUtils::checkFileExtension(filename, ".txt") ||
          StringUtils::checkFileExtension(filename, ".md")) {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);
}

size_t MyLibraryActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++) {
    if (files[i] == name) return i;
  }
  return 0;
}

void MyLibraryActivity::taskTrampoline(void* param) {
  auto* self = static_cast<MyLibraryActivity*>(param);
  self->displayTaskLoop();
}

// Action execution: Move one item (short press timeout)
void MyLibraryActivity::executeMoveItem(bool isPrevButton) {
  const int itemCount = getCurrentItemCount();
  if (itemCount > 0) {
    if (isPrevButton) {
      selectorIndex = (selectorIndex + itemCount - 1) % itemCount;
    } else {
      selectorIndex = (selectorIndex + 1) % itemCount;
    }
  }
}

// Action execution: Skip page (double press)
void MyLibraryActivity::executeSkipPage(bool isPrevButton) {
  const int itemCount = getCurrentItemCount();
  const int pageItems = getPageItems();
  if (itemCount > 0) {
    if (isPrevButton) {
      int targetPage = (selectorIndex / pageItems) - 1;
      if (targetPage < 0) {
        targetPage = ((itemCount - 1) / pageItems);
      }
      selectorIndex = targetPage * pageItems;
    } else {
      int targetPage = (selectorIndex / pageItems) + 1;
      int maxPage = (itemCount - 1) / pageItems;
      if (targetPage > maxPage) {
        targetPage = 0;
      }
      selectorIndex = targetPage * pageItems;
    }
  }
}

// Action execution: Switch tab (long press)
void MyLibraryActivity::executeSwitchTab(bool isPrevButton) {
  if (isPrevButton && currentTab == Tab::Files) {
    currentTab = Tab::Recent;
    selectorIndex = 0;
  } else if (!isPrevButton && currentTab == Tab::Recent) {
    currentTab = Tab::Files;
    selectorIndex = 0;
  }
}

void MyLibraryActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Load data for both tabs
  loadRecentBooks();
  loadFiles();

  selectorIndex = 0;
  updateRequired = true;

  xTaskCreate(&MyLibraryActivity::taskTrampoline, "MyLibraryActivityTask",
              4096,               // Stack size (increased for epub metadata loading)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void MyLibraryActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to
  // EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  bookTitles.clear();
  bookPaths.clear();
  files.clear();
}

void MyLibraryActivity::loop() {
  // Get current time for all timing operations
  unsigned long currentTime = millis();

  // Long press BACK (1s+) in Files tab goes to root folder
  if (currentTab == Tab::Files && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= GO_HOME_MS) {
    if (basepath != "/") {
      basepath = "/";
      loadFiles();
      selectorIndex = 0;
      updateRequired = true;
    }
    return;
  }

  // Confirm button - open selected item
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (currentTab == Tab::Recent) {
      if (!bookPaths.empty() && selectorIndex < static_cast<int>(bookPaths.size())) {
        onSelectBook(bookPaths[selectorIndex], currentTab);
      }
    } else {
      // Files tab
      if (!files.empty() && selectorIndex < static_cast<int>(files.size())) {
        if (basepath.back() != '/') basepath += "/";
        if (files[selectorIndex].back() == '/') {
          // Enter directory
          basepath += files[selectorIndex].substr(0, files[selectorIndex].length() - 1);
          loadFiles();
          selectorIndex = 0;
          updateRequired = true;
        } else {
          // Open file
          onSelectBook(basepath + files[selectorIndex], currentTab);
        }
      }
    }
    return;
  }

  // Back button
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (currentTab == Tab::Files && basepath != "/") {
        // Go up one directory, remembering the directory we came from
        const std::string oldPath = basepath;
        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();

        // Select the directory we just came from
        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = static_cast<int>(findEntry(dirName));

        updateRequired = true;
      } else {
        // Go home
        onGoHome();
      }
    }
    return;
  }

  // Navigation buttons (UP/LEFT and DOWN/RIGHT have same behavior)
  const bool upPressed = mappedInput.isPressed(MappedInputManager::Button::Up);
  const bool leftPressed = mappedInput.isPressed(MappedInputManager::Button::Left);
  const bool downPressed = mappedInput.isPressed(MappedInputManager::Button::Down);
  const bool rightPressed = mappedInput.isPressed(MappedInputManager::Button::Right);
  const bool upReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool leftReleased = mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool downReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
  const bool rightReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);

  // Navigation: UP/LEFT move backward, DOWN/RIGHT move forward
  const bool prevPressed = upPressed || leftPressed;
  const bool nextPressed = downPressed || rightPressed;
  const bool prevReleased = upReleased || leftReleased;
  const bool nextReleased = downReleased || rightReleased;

  // State machine for button press detection
  // ==========================================

  // IDLE: Wait for first press
  if (buttonState == ButtonState::Idle) {
    if (prevPressed || nextPressed) {
      buttonState = ButtonState::FirstPress;
      firstPressTime = currentTime;
      isPrevButtonPressed = prevPressed;
    }
  }
  // FIRST_PRESS: Button is held, check for long press or release
  else if (buttonState == ButtonState::FirstPress) {
    const unsigned long holdDuration = currentTime - firstPressTime;

    // Check for long press (>=450ms) - switch tab
    if (holdDuration >= LONG_PRESS_MS) {
      executeSwitchTab(isPrevButtonPressed);
      buttonState = ButtonState::WaitingForReleaseAfterLongPress;
      updateRequired = true;
      return;
    }

    // Check for release (<450ms) - transition to waiting for second press
    if ((isPrevButtonPressed && prevReleased) || (!isPrevButtonPressed && nextReleased)) {
      buttonState = ButtonState::WaitingForSecondPress;
      firstReleaseTime = currentTime;
    }
  }
  // WAITING_FOR_SECOND_PRESS: First button released, waiting for second press
  else if (buttonState == ButtonState::WaitingForSecondPress) {
    const unsigned long waitDuration = currentTime - firstReleaseTime;

    // Timeout (>=120ms without second press) - execute move_item
    if (waitDuration >= DOUBLE_PRESS_MS) {
      executeMoveItem(isPrevButtonPressed);
      buttonState = ButtonState::Idle;
      updateRequired = true;
      return;
    }

    // Second press detected (<120ms) - double press
    if (prevPressed || nextPressed) {
      buttonState = ButtonState::DoublePressDetected;
    }
  }
  // DOUBLE_PRESS_DETECTED: Second press detected, wait for release
  else if (buttonState == ButtonState::DoublePressDetected) {
    // Wait for second button release
    if (prevReleased || nextReleased) {
      executeSkipPage(isPrevButtonPressed);
      buttonState = ButtonState::Idle;
      updateRequired = true;
      return;
    }
  }
  // WAITING_FOR_RELEASE_AFTER_LONG_PRESS: Ignore release after long press
  else if (buttonState == ButtonState::WaitingForReleaseAfterLongPress) {
    // Wait for button to be released, then go back to idle
    if ((isPrevButtonPressed && prevReleased) || (!isPrevButtonPressed && nextReleased)) {
      buttonState = ButtonState::Idle;
    }
  }
}

void MyLibraryActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void MyLibraryActivity::render() const {
  renderer.clearScreen();

  // Draw tab bar
  std::vector<TabInfo> tabs = {{"Recent", currentTab == Tab::Recent}, {"Files", currentTab == Tab::Files}};
  ScreenComponents::drawTabBar(renderer, TAB_BAR_Y, tabs);

  // Draw content based on current tab
  if (currentTab == Tab::Recent) {
    renderRecentTab();
  } else {
    renderFilesTab();
  }

  // Draw scroll indicator
  const int screenHeight = renderer.getScreenHeight();
  const int contentHeight = screenHeight - CONTENT_START_Y - 60;  // 60 for bottom bar
  ScreenComponents::drawScrollIndicator(renderer, getCurrentPage(), getTotalPages(), CONTENT_START_Y, contentHeight);

  // Draw side button hints (up/down navigation on right side)
  // Note: text is rotated 90° CW, so ">" appears as "^" and "<" appears as "v"
  renderer.drawSideButtonHints(UI_10_FONT_ID, ">", "<");

  // Draw bottom button hints
  const auto labels = mappedInput.mapLabels("« Back", "Open", "<", ">");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void MyLibraryActivity::renderRecentTab() const {
  const auto pageWidth = renderer.getScreenWidth();
  const int pageItems = getPageItems();
  const int bookCount = static_cast<int>(bookTitles.size());

  if (bookCount == 0) {
    renderer.drawText(UI_10_FONT_ID, LEFT_MARGIN, CONTENT_START_Y, "No recent books");
    return;
  }

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;

  // Draw selection highlight
  renderer.fillRect(0, CONTENT_START_Y + (selectorIndex % pageItems) * LINE_HEIGHT - 2, pageWidth - RIGHT_MARGIN,
                    LINE_HEIGHT);

  // Draw items
  for (int i = pageStartIndex; i < bookCount && i < pageStartIndex + pageItems; i++) {
    auto item = renderer.truncatedText(UI_10_FONT_ID, bookTitles[i].c_str(), pageWidth - LEFT_MARGIN - RIGHT_MARGIN);
    renderer.drawText(UI_10_FONT_ID, LEFT_MARGIN, CONTENT_START_Y + (i % pageItems) * LINE_HEIGHT, item.c_str(),
                      i != selectorIndex);
  }
}

void MyLibraryActivity::renderFilesTab() const {
  const auto pageWidth = renderer.getScreenWidth();
  const int pageItems = getPageItems();
  const int fileCount = static_cast<int>(files.size());

  if (fileCount == 0) {
    renderer.drawText(UI_10_FONT_ID, LEFT_MARGIN, CONTENT_START_Y, "No books found");
    return;
  }

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;

  // Draw selection highlight
  renderer.fillRect(0, CONTENT_START_Y + (selectorIndex % pageItems) * LINE_HEIGHT - 2, pageWidth - RIGHT_MARGIN,
                    LINE_HEIGHT);

  // Draw items
  for (int i = pageStartIndex; i < fileCount && i < pageStartIndex + pageItems; i++) {
    auto item = renderer.truncatedText(UI_10_FONT_ID, files[i].c_str(), pageWidth - LEFT_MARGIN - RIGHT_MARGIN);
    renderer.drawText(UI_10_FONT_ID, LEFT_MARGIN, CONTENT_START_Y + (i % pageItems) * LINE_HEIGHT, item.c_str(),
                      i != selectorIndex);
  }
}
