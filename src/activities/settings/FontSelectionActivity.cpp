#include "FontSelectionActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "FontManager.h"
#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
constexpr const char* DEFAULT_FONT_NAME = "Default";
constexpr const char* CACHE_DIR = "/.crosspoint/cache";

// Recursively delete a directory and its contents
void deleteDirectory(const char* path) {
  FsFile dir = SdMan.open(path);
  if (!dir || !dir.isDir()) {
    if (dir) dir.close();
    return;
  }

  FsFile entry;
  while (entry.openNext(&dir, O_RDONLY)) {
    char entryName[64];
    entry.getName(entryName, sizeof(entryName));
    entry.close();

    std::string fullPath = std::string(path) + "/" + entryName;
    FsFile check = SdMan.open(fullPath.c_str());
    if (check) {
      bool isDir = check.isDir();
      check.close();
      if (isDir) {
        deleteDirectory(fullPath.c_str());
      } else {
        SdMan.remove(fullPath.c_str());
      }
    }
  }
  dir.close();
  SdMan.rmdir(path);
}

// Invalidate rendering caches for EPUB and TXT readers
// Keeps progress.bin (reading position) but removes layout caches
void invalidateReaderCaches() {
  Serial.printf("[%lu] [FNT] Invalidating reader rendering caches...\n", millis());

  FsFile cacheDir = SdMan.open(CACHE_DIR);
  if (!cacheDir || !cacheDir.isDir()) {
    if (cacheDir) cacheDir.close();
    Serial.printf("[%lu] [FNT] No cache directory found\n", millis());
    return;
  }

  int deletedCount = 0;
  FsFile bookCache;
  while (bookCache.openNext(&cacheDir, O_RDONLY)) {
    char bookCacheName[64];
    bookCache.getName(bookCacheName, sizeof(bookCacheName));
    bookCache.close();

    std::string bookCachePath = std::string(CACHE_DIR) + "/" + bookCacheName;

    // For EPUB: delete sections/ folder (keeps progress.bin)
    std::string sectionsPath = bookCachePath + "/sections";
    FsFile sectionsDir = SdMan.open(sectionsPath.c_str());
    if (sectionsDir && sectionsDir.isDir()) {
      sectionsDir.close();
      deleteDirectory(sectionsPath.c_str());
      Serial.printf("[%lu] [FNT] Deleted EPUB sections cache: %s\n", millis(), sectionsPath.c_str());
      deletedCount++;
    } else {
      if (sectionsDir) sectionsDir.close();
    }

    // For TXT: delete index.bin (keeps progress.bin)
    std::string indexPath = bookCachePath + "/index.bin";
    if (SdMan.exists(indexPath.c_str())) {
      SdMan.remove(indexPath.c_str());
      Serial.printf("[%lu] [FNT] Deleted TXT index cache: %s\n", millis(), indexPath.c_str());
      deletedCount++;
    }
  }
  cacheDir.close();

  Serial.printf("[%lu] [FNT] Invalidated %d cache entries\n", millis(), deletedCount);
}
}  // namespace

void FontSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<FontSelectionActivity*>(param);
  self->displayTaskLoop();
}

void FontSelectionActivity::loadFontList() {
  fontFiles.clear();
  fontNames.clear();

  // First entry is always the default font (empty path means default)
  fontFiles.emplace_back("");
  fontNames.emplace_back(DEFAULT_FONT_NAME);

  // Ensure fonts directory exists
  SdMan.mkdir("/.crosspoint");
  SdMan.mkdir(FONTS_DIR);

  // Try to open the fonts folder
  FsFile dir = SdMan.open(FONTS_DIR);
  if (!dir) {
    Serial.printf("[%lu] [FNT] Font folder %s not found\n", millis(), FONTS_DIR);
    return;
  }

  if (!dir.isDir()) {
    Serial.printf("[%lu] [FNT] %s is not a directory\n", millis(), FONTS_DIR);
    dir.close();
    return;
  }

  // List all .epdfont files
  FsFile file;
  while (file.openNext(&dir, O_RDONLY)) {
    if (!file.isDir()) {
      char filename[64];
      file.getName(filename, sizeof(filename));

      // Check if file has .epdfont extension and skip macOS hidden files (._*)
      const size_t len = strlen(filename);
      if (len > 8 && strcasecmp(filename + len - 8, ".epdfont") == 0 && strncmp(filename, "._", 2) != 0) {
        // Build full path
        std::string fullPath = std::string(FONTS_DIR) + "/" + filename;
        fontFiles.push_back(fullPath);

        // Extract name without extension for display
        std::string displayName(filename, len - 8);
        fontNames.push_back(displayName);

        Serial.printf("[%lu] [FNT] Found font: %s\n", millis(), fullPath.c_str());
      }
    }
    file.close();
  }
  dir.close();

  Serial.printf("[%lu] [FNT] Total fonts found: %zu (including default)\n", millis(), fontFiles.size());

  // Find currently selected font index
  selectedIndex = 0;  // Default
  if (SETTINGS.hasCustomFont()) {
    for (size_t i = 1; i < fontFiles.size(); i++) {
      if (fontFiles[i] == SETTINGS.customFontPath) {
        selectedIndex = static_cast<int>(i);
        break;
      }
    }
  }
}

void FontSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Load font list from SD card
  loadFontList();

  updateRequired = true;

  xTaskCreate(&FontSelectionActivity::taskTrampoline, "FontSelectionTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void FontSelectionActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void FontSelectionActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int itemCount = static_cast<int>(fontNames.size());
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + itemCount - 1) % itemCount;
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % itemCount;
    updateRequired = true;
  }
}

void FontSelectionActivity::handleSelection() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  // Show loading screen
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 - 10, "Applying font...");
  renderer.displayBuffer();

  // Update custom font path in settings
  if (selectedIndex == 0) {
    // Default font selected - clear custom font path
    SETTINGS.customFontPath[0] = '\0';
  } else {
    // Custom font selected
    strncpy(SETTINGS.customFontPath, fontFiles[selectedIndex].c_str(), sizeof(SETTINGS.customFontPath) - 1);
    SETTINGS.customFontPath[sizeof(SETTINGS.customFontPath) - 1] = '\0';
  }

  SETTINGS.saveToFile();
  Serial.printf("[%lu] [FNT] Font selected: %s\n", millis(), selectedIndex == 0 ? "default" : SETTINGS.customFontPath);

  // Reload custom font dynamically (no reboot needed)
  reloadCustomReaderFont();

  // Invalidate EPUB/TXT caches since font changed
  invalidateReaderCaches();

  xSemaphoreGive(renderingMutex);

  // Return to settings
  onBack();
}

void FontSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void FontSelectionActivity::render() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Custom Font", true, EpdFontFamily::BOLD);

  // Calculate visible items (with scrolling if needed)
  constexpr int lineHeight = 30;
  constexpr int startY = 60;
  const int maxVisibleItems = (pageHeight - startY - 50) / lineHeight;
  const int itemCount = static_cast<int>(fontNames.size());

  // Calculate scroll offset to keep selected item visible
  int scrollOffset = 0;
  if (itemCount > maxVisibleItems) {
    if (selectedIndex >= maxVisibleItems) {
      scrollOffset = selectedIndex - maxVisibleItems + 1;
    }
  }

  // Determine current selection (for checkmark comparison)
  int currentSelectedIndex = 0;  // Default
  if (SETTINGS.hasCustomFont()) {
    for (size_t i = 1; i < fontFiles.size(); i++) {
      if (fontFiles[i] == SETTINGS.customFontPath) {
        currentSelectedIndex = static_cast<int>(i);
        break;
      }
    }
  }

  // Draw font list
  for (int i = 0; i < maxVisibleItems && (i + scrollOffset) < itemCount; i++) {
    const int itemIndex = i + scrollOffset;
    const int itemY = startY + i * lineHeight;
    const bool isHighlighted = (itemIndex == selectedIndex);
    const bool isCurrentFont = (itemIndex == currentSelectedIndex);

    // Draw selection highlight
    if (isHighlighted) {
      renderer.fillRect(0, itemY - 2, pageWidth - 1, lineHeight);
    }

    // Draw checkmark for currently active font (using asterisk - available in Pretendard)
    if (isCurrentFont) {
      renderer.drawText(UI_10_FONT_ID, 10, itemY, "*", !isHighlighted);
    }

    // Draw font name
    renderer.drawText(UI_10_FONT_ID, 35, itemY, fontNames[itemIndex].c_str(), !isHighlighted);
  }

  // Draw scroll indicators if needed
  if (scrollOffset > 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, startY - 15, "...", true);
  }
  if (scrollOffset + maxVisibleItems < itemCount) {
    renderer.drawCenteredText(UI_10_FONT_ID, startY + maxVisibleItems * lineHeight, "...", true);
  }

  // Draw help text
  const auto labels = mappedInput.mapLabels("« Back", "Select", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
