#include "GridBrowserActivity.h"

#include <GfxRenderer.h>
#include <SD.h>
#include <InputManager.h>

#include "config.h"
#include "../../images/FolderIcon.h"
#include "../util/Window.h"

namespace {
constexpr int PAGE_ITEMS = 9;
constexpr int SKIP_PAGE_MS = 700;
constexpr int TILE_W = 135;
constexpr int TILE_H = 200;
constexpr int TILE_PADDING = 5;
constexpr int THUMB_W = 90;
constexpr int THUMB_H = 120;
constexpr int TILE_TEXT_H = 60;
constexpr int gridLeftOffset = 45;
constexpr int gridTopOffset = 125;
}  // namespace

inline int min(const int a, const int b) { return a < b ? a : b; }

void GridBrowserActivity::sortFileList(std::vector<FileInfo>& strs) {
  std::sort(begin(strs), end(strs), [](const FileInfo& f1, const FileInfo& f2) {
    if (f1.type == F_DIRECTORY && f2.type != F_DIRECTORY) return true;
    if (f1.type != F_DIRECTORY && f2.type == F_DIRECTORY) return false;
    return lexicographical_compare(
        begin(f1.name), end(f1.name), begin(f2.name), end(f2.name),
        [](const char& char1, const char& char2) { return tolower(char1) < tolower(char2); });
  });
}

void GridBrowserActivity::taskTrampoline(void* param) {
  auto* self = static_cast<GridBrowserActivity*>(param);
  self->displayTaskLoop();
}

void GridBrowserActivity::loadFiles() {
  files.clear();
  selectorIndex = 0;
  previousSelectorIndex = -1;
  page = 0;
  auto root = SD.open(basepath.c_str());
  for (File file = root.openNextFile(); file; file = root.openNextFile()) {
    const std::string filename = std::string(file.name());
    if (filename.empty() || filename[0] == '.') {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(FileInfo{ filename, filename, F_DIRECTORY });
    } else {
      FileType type = F_FILE;
      size_t dot = filename.find_first_of('.');
      std::string basename = filename;
      if (dot != std::string::npos) {
        std::string ext = filename.substr(dot);
        basename = filename.substr(0, dot); 
        // lowercase ext for case-insensitive compare
        for (char &c : ext) c = (char)tolower(c);
        if (ext == ".epub") {
          type = F_EPUB;
        } else if (ext == ".thumb.bmp") {
          type = F_BMP;
        }
      }
      if (type != F_FILE) {
        files.emplace_back(FileInfo{ filename, basename, type });
      }
    }
    file.close();
  }
  root.close();
  Serial.printf("Files loaded\n");
  GridBrowserActivity::sortFileList(files);
  Serial.printf("Files sorted\n");
}

void GridBrowserActivity::onEnter() {
  Serial.printf("Enter grid\n");
  renderingMutex = xSemaphoreCreateMutex();
  
  basepath = "/";
  loadFiles();
  selectorIndex = 0;
  page = 0;
  
  // Trigger first render
  renderRequired = true;
  
  xTaskCreate(&GridBrowserActivity::taskTrampoline, "GridFileBrowserTask",
              8192,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void GridBrowserActivity::onExit() {
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

void GridBrowserActivity::loop() {
  const bool prevReleased = inputManager.wasReleased(InputManager::BTN_UP) || inputManager.wasReleased(InputManager::BTN_LEFT);
  const bool nextReleased = inputManager.wasReleased(InputManager::BTN_DOWN) || inputManager.wasReleased(InputManager::BTN_RIGHT);
  const bool skipPage = inputManager.getHeldTime() > SKIP_PAGE_MS;
  const int selected = selectorIndex + page * PAGE_ITEMS;

  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    if (files.empty()) {
      return;
    }

    if (basepath.back() != '/') {
      basepath += "/";
    }
    if (files[selected].type == F_DIRECTORY) {
      // open subfolder
      basepath += files[selected].name;
      loadFiles();
      renderRequired = true;
    } else {
      onSelect(basepath + files[selected].name);
    }
  } else if (inputManager.wasPressed(InputManager::BTN_BACK)) {
    if (basepath != "/") {
      basepath = basepath.substr(0, basepath.rfind('/'));
      if (basepath.empty()) basepath = "/";
      loadFiles();
      renderRequired = true;
    } else {
      // At root level, go back home
      onGoHome();
    }
  } else if (prevReleased) {
    previousSelectorIndex = selectorIndex;
    if (selectorIndex == 0 || skipPage) {
      if (page > 0) {
        page--;
        selectorIndex = 0;
        previousSelectorIndex = -1;
        renderRequired = true;
      }
    } else {
      selectorIndex--;
      updateRequired = true;
    }
  } else if (nextReleased) {
    previousSelectorIndex = selectorIndex;
    if (selectorIndex == min(PAGE_ITEMS, files.size() - page * PAGE_ITEMS) - 1 || skipPage) {
      if (page < files.size() / PAGE_ITEMS) {
        page++;
        selectorIndex = 0;
        previousSelectorIndex = -1;
        renderRequired = true;
      }
    } else {
      selectorIndex++;
      updateRequired = true;
    }
  }
}

void GridBrowserActivity::displayTaskLoop() {
  while (true) {
    if (renderRequired) {
      renderRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render(true);
      xSemaphoreGive(renderingMutex);
    } else if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      // update(true);
      render(false);
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void GridBrowserActivity::render(bool clear) const {
  if (clear) {
    renderer.clearScreen();
    auto folderName = basepath == "/" ? "SD card" : basepath.substr(basepath.rfind('/') + 1).c_str();
    drawFullscreenWindowFrame(renderer, folderName);
  }
  bool hasGeyscaleBitmaps = false;
  
  if (!files.empty()) {
    for (int pass = 0; pass < 3; pass++) {
      if (pass > 0) {
        renderer.clearScreen(0x00);
        if (pass == 1) {
          renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        } else if (pass == 2) {
          renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        }
      }

      for (size_t i = 0; i < min(PAGE_ITEMS, files.size() - page * PAGE_ITEMS); i++) {
        const auto file = files[i + page * PAGE_ITEMS];
        
        const int16_t tileX = gridLeftOffset + i % 3 * TILE_W;
        const int16_t tileY = gridTopOffset + i / 3 * TILE_H;

        if (pass == 0) {
          if (file.type == F_DIRECTORY) {
            constexpr int iconOffsetX = (TILE_W - FOLDERICON_WIDTH) / 2;
            constexpr int iconOffsetY = (TILE_H - TILE_TEXT_H - FOLDERICON_HEIGHT) / 2;
            renderer.drawIcon(FolderIcon, tileX + iconOffsetX, tileY + iconOffsetY, FOLDERICON_WIDTH, FOLDERICON_HEIGHT);
          }
        }

        if (file.type == F_BMP) {
          File bmpFile = SD.open((basepath + "/" + file.name).c_str());
          if (bmpFile) {
            Bitmap bitmap(bmpFile);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              if (bitmap.hasGreyscale()) {
                hasGeyscaleBitmaps = true;
              }
              constexpr int thumbOffsetX = (TILE_W - THUMB_W) / 2;
              constexpr int thumbOffsetY = (TILE_H - TILE_TEXT_H - THUMB_H) / 2;
              renderer.drawBitmap(bitmap, tileX + thumbOffsetX, tileY + thumbOffsetY, THUMB_W, THUMB_H);
            }
          }
        }
        
        if (pass == 0) {
          renderer.drawTextInBox(UI_FONT_ID, tileX + TILE_PADDING, tileY + TILE_H - TILE_TEXT_H, TILE_W - 2 * TILE_PADDING, TILE_TEXT_H, file.basename.c_str(), true);
        }
      }

      if (pass == 0) {
        update(false);
        renderer.displayBuffer();
        if (hasGeyscaleBitmaps) {
          renderer.storeBwBuffer();
        } else {
          // we can skip grayscale passes if no bitmaps use it
          break;
        }
      } else if (pass == 1) {
        renderer.copyGrayscaleLsbBuffers();
      } else if (pass == 2) {
        renderer.copyGrayscaleMsbBuffers();
        renderer.displayGrayBuffer();
        renderer.setRenderMode(GfxRenderer::BW);
        renderer.restoreBwBuffer();
      }
    }
  }
} 

void GridBrowserActivity::drawSelectionRectangle(int tileIndex, bool black) const {
  renderer.drawRoundedRect(gridLeftOffset + tileIndex % 3 * TILE_W, gridTopOffset + tileIndex / 3 * TILE_H, TILE_W, TILE_H, 2, 5, black);
}

void GridBrowserActivity::update(bool render) const {
  // Redraw only changed tiles
  // renderer.clearScreen();
  if (previousSelectorIndex >= 0) {
    drawSelectionRectangle(previousSelectorIndex, false);
  }
  drawSelectionRectangle(selectorIndex, true);
}