#include "CoverArtPickerActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <Xtc.h>

#include "Bitmap.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
constexpr int GRID_COLS = 3;
constexpr int GRID_ROWS = 4;
constexpr int PAGE_ITEMS = GRID_COLS * GRID_ROWS;  // 12 items per page
constexpr int CELL_WIDTH = 160;
constexpr int CELL_HEIGHT = 180;
constexpr int COVER_WIDTH = 120;   // Leave some spacing
constexpr int COVER_HEIGHT = 160;  // Leave some spacing
constexpr int GRID_START_Y = 50;
constexpr int SKIP_PAGE_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;
}  // namespace

void sortFileList(std::vector<std::string>& strs);  // Declared in FileSelectionActivity.cpp

void CoverArtPickerActivity::taskTrampoline(void* param) {
  auto* self = static_cast<CoverArtPickerActivity*>(param);
  self->displayTaskLoop();
}

void CoverArtPickerActivity::loadFiles() {
  files.clear();
  selectorIndex = 0;

  auto root = SdMan.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  char name[128];
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
      std::string ext4 = filename.length() >= 4 ? filename.substr(filename.length() - 4) : "";
      std::string ext5 = filename.length() >= 5 ? filename.substr(filename.length() - 5) : "";
      if (ext5 == ".epub" || ext5 == ".xtch" || ext4 == ".xtc") {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);
}

void CoverArtPickerActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  loadFiles();
  selectorIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&CoverArtPickerActivity::taskTrampoline, "CoverArtPickerActivityTask",
              4096,               // Stack size (need more for EPUB loading)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void CoverArtPickerActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  files.clear();
}

void CoverArtPickerActivity::loop() {
  // Long press BACK (1s+) goes to root folder
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS) {
    if (basepath != "/") {
      basepath = "/";
      APP_STATE.lastBrowsedFolder = basepath;
      APP_STATE.saveToFile();
      loadFiles();
      updateRequired = true;
    }
    return;
  }

  const bool leftPressed = mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool rightPressed = mappedInput.wasReleased(MappedInputManager::Button::Right);
  const bool upPressed = mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool downPressed = mappedInput.wasReleased(MappedInputManager::Button::Down);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (files.empty()) {
      return;
    }

    if (basepath.back() != '/') basepath += "/";
    if (files[selectorIndex].back() == '/') {
      basepath += files[selectorIndex].substr(0, files[selectorIndex].length() - 1);
      APP_STATE.lastBrowsedFolder = basepath;
      APP_STATE.saveToFile();
      loadFiles();
      updateRequired = true;
    } else {
      onSelect(basepath + files[selectorIndex]);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        APP_STATE.lastBrowsedFolder = basepath;
        APP_STATE.saveToFile();
        loadFiles();
        updateRequired = true;
      } else {
        onGoHome();
      }
    }
  } else if (leftPressed) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / PAGE_ITEMS - 1) * PAGE_ITEMS + files.size()) % files.size();
    } else {
      selectorIndex = (selectorIndex + files.size() - 1) % files.size();
    }
    updateRequired = true;
  } else if (rightPressed) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / PAGE_ITEMS + 1) * PAGE_ITEMS) % files.size();
    } else {
      selectorIndex = (selectorIndex + 1) % files.size();
    }
    updateRequired = true;
  } else if (upPressed) {
    // Move up one row
    selectorIndex = (selectorIndex - GRID_COLS + files.size()) % files.size();
    updateRequired = true;
  } else if (downPressed) {
    // Move down one row
    selectorIndex = (selectorIndex + GRID_COLS) % files.size();
    updateRequired = true;
  }
}

void CoverArtPickerActivity::displayTaskLoop() {
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

void CoverArtPickerActivity::drawCoverThumbnail(const std::string& filePath, int gridX, int gridY,
                                                bool selected) const {
  const int x = gridX * CELL_WIDTH + (CELL_WIDTH - COVER_WIDTH) / 2;
  const int y = GRID_START_Y + gridY * CELL_HEIGHT + (CELL_HEIGHT - COVER_HEIGHT) / 2;

  // Draw selection box
  if (selected) {
    renderer.drawRect(x - 2, y - 2, COVER_WIDTH + 4, COVER_HEIGHT + 4);
  }

  // If it's a directory, draw a folder icon
  if (filePath.back() == '/') {
    std::string dirName = filePath.substr(0, filePath.length() - 1);
    if (dirName.length() > 12) {
      dirName = dirName.substr(0, 12) + "...";
    }

    // Draw a folder outline (like a book but with a tab)
    const int folderX = x + 30;
    const int folderY = y + 30;
    const int folderW = COVER_WIDTH - 60;
    const int folderH = COVER_HEIGHT - 80;

    // Main folder body
    renderer.drawRect(folderX, folderY + 10, folderW, folderH - 10);
    // Folder tab
    renderer.drawRect(folderX, folderY, folderW / 2, 10);

    // Draw folder label with background if selected
    const int labelY = y + COVER_HEIGHT - 25;
    const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, dirName.c_str());
    const int labelX = x + (COVER_WIDTH - labelWidth) / 2;

    if (selected) {
      // Fill background for selected text
      renderer.fillRect(labelX - 2, labelY - 2, labelWidth + 4, renderer.getLineHeight(SMALL_FONT_ID) + 4);
    }

    // Always draw black text on white background, or white text on black background
    renderer.drawText(SMALL_FONT_ID, labelX, labelY, dirName.c_str(), !selected);

    return;
  }

  // Prepare display name
  std::string displayName = filePath;
  if (displayName.length() > 5 && displayName.substr(displayName.length() - 5) == ".epub") {
    displayName = displayName.substr(0, displayName.length() - 5);
  } else if (displayName.length() > 5 && displayName.substr(displayName.length() - 5) == ".xtch") {
    displayName = displayName.substr(0, displayName.length() - 5);
  } else if (displayName.length() > 4 && displayName.substr(displayName.length() - 4) == ".xtc") {
    displayName = displayName.substr(0, displayName.length() - 4);
  }
  if (displayName.length() > 15) {
    displayName = displayName.substr(0, 15) + "...";
  }

  // Build full path
  std::string fullPath = basepath;
  if (fullPath.back() != '/') fullPath += "/";
  fullPath += filePath;

  // Determine if XTC or EPUB
  bool isXtc = false;
  if (filePath.length() >= 4) {
    std::string ext4 = filePath.substr(filePath.length() - 4);
    if (ext4 == ".xtc") isXtc = true;
  }
  if (!isXtc && filePath.length() >= 5) {
    std::string ext5 = filePath.substr(filePath.length() - 5);
    if (ext5 == ".xtch") isXtc = true;
  }

  // Try to get cover using the same pattern as sleep screen
  std::string coverBmpPath;
  bool coverGenerated = false;

  if (isXtc) {
    Xtc book(fullPath, "/.crosspoint");
    if (book.load()) {
      if (book.generateCoverBmp()) {  // This is fast if cover already exists
        coverBmpPath = book.getCoverBmpPath();
        coverGenerated = true;
      }
    }
  } else {
    Epub book(fullPath, "/.crosspoint");
    if (book.load(false)) {  // Load without building metadata if missing
      if (book.generateCoverBmp()) {  // This is fast if cover already exists
        coverBmpPath = book.getCoverBmpPath();
        coverGenerated = true;
      }
    }
  }

  // Render the cover if we got one
  if (coverGenerated) {
    FsFile coverFile;
    if (SdMan.openFileForRead("COVER", coverBmpPath.c_str(), coverFile)) {
      Bitmap coverBitmap(coverFile);
      if (coverBitmap.parseHeaders() == BmpReaderError::Ok) {
        renderer.drawBitmap(coverBitmap, x, y, COVER_WIDTH, COVER_HEIGHT);
        // Don't close file here - Bitmap holds a reference to it and needs it open
        // File will close when coverFile goes out of scope
        return;  // Success - cover rendered
      }
    }
  }

  // Fallback: show filename with border
  renderer.drawRect(x + 20, y + 20, COVER_WIDTH - 40, COVER_HEIGHT - 60);

  // Draw filename centered within the cell (not screen-wide)
  const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, displayName.c_str());
  const int textX = x + (COVER_WIDTH - textWidth) / 2;
  renderer.drawText(SMALL_FONT_ID, textX, y + COVER_HEIGHT - 30, displayName.c_str(), true);
}

void CoverArtPickerActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Books", true, BOLD);

  // Help text
  const auto labels = mappedInput.mapLabels("« Home", "Open", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, 20, 60, "No books found");
    renderer.displayBuffer();
    return;
  }

  // Calculate page
  const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;

  // Draw covers in grid
  for (int i = pageStartIndex; i < files.size() && i < pageStartIndex + PAGE_ITEMS; i++) {
    const int gridIndex = i - pageStartIndex;
    const int gridX = gridIndex % GRID_COLS;
    const int gridY = gridIndex / GRID_COLS;
    const bool selected = (i == selectorIndex);

    drawCoverThumbnail(files[i], gridX, gridY, selected);
  }

  renderer.displayBuffer();
}
