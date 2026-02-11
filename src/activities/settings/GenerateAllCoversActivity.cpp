#include "GenerateAllCoversActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Utf8.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr int MAX_SCAN_DEPTH = 10;
constexpr unsigned long DISPLAY_UPDATE_INTERVAL_MS = 5000;  // Update every 5 seconds
constexpr int BOOKS_PER_REFRESH = 3;                        // Or every 3 books
constexpr int SCREEN_MARGIN = 20;
constexpr int LINE_HEIGHT = 30;
}  // namespace

void GenerateAllCoversActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  Serial.printf("[%lu] [GAC] GenerateAllCoversActivity::onEnter()\n", millis());
  startTime = millis();
  lastRefreshTime = millis();
  currentState = SCANNING;

  // Show initial scanning message
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2 - 40, "Scanning library...");
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, "Please wait");
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  // Scan for all EPUB files
  scanLibraryForEpubs();

  totalBooks = epubFiles.size();
  Serial.printf("[%lu] [GAC] Found %d EPUB files\n", millis(), totalBooks);

  if (totalBooks == 0) {
    // No books found
    currentState = COMPLETE;
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2 - 20, "No EPUB files found");
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 + 20, "Press Back to return");
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  // Start generating
  currentState = GENERATING;
  renderProgress();
}

void GenerateAllCoversActivity::loop() {
  // Handle cancellation
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (currentState == GENERATING) {
      shouldCancel = true;
      currentState = CANCELLED;
      Serial.printf("[%lu] [GAC] User cancelled generation\n", millis());
      renderSummary();
    } else if (currentState == COMPLETE || currentState == CANCELLED) {
      goBack();
    }
    return;
  }

  // Process books
  if (currentState == GENERATING && !shouldCancel) {
    if (currentIndex < totalBooks) {
      generateCoversForBook(epubFiles[currentIndex]);
      currentIndex++;

      // Update display periodically
      bool shouldRefresh = false;
      if (currentIndex % BOOKS_PER_REFRESH == 0) {
        shouldRefresh = true;
      }
      if (millis() - lastRefreshTime > DISPLAY_UPDATE_INTERVAL_MS) {
        shouldRefresh = true;
      }

      if (shouldRefresh) {
        renderProgress();
        lastRefreshTime = millis();
      }
    } else {
      // All books processed
      currentState = COMPLETE;
      Serial.printf("[%lu] [GAC] Generation complete\n", millis());
      renderSummary();
    }
  }
}

void GenerateAllCoversActivity::onExit() {
  ActivityWithSubactivity::onExit();
  Serial.printf("[%lu] [GAC] GenerateAllCoversActivity::onExit()\n", millis());
}

void GenerateAllCoversActivity::scanLibraryForEpubs() {
  epubFiles.clear();
  scanDirectoryRecursive("/", 0);
  Serial.printf("[%lu] [GAC] Scan complete, found %d files\n", millis(), epubFiles.size());
}

void GenerateAllCoversActivity::scanDirectoryRecursive(const char* path, int depth) {
  if (depth > MAX_SCAN_DEPTH) {
    Serial.printf("[%lu] [GAC] Max scan depth reached at: %s\n", millis(), path);
    return;
  }

  // Skip .crosspoint cache directory
  if (strstr(path, "/.crosspoint") != nullptr || strstr(path, ".crosspoint") != nullptr) {
    return;
  }

  Serial.printf("[%lu] [GAC] Scanning directory: %s (depth %d)\n", millis(), path, depth);

  auto dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    Serial.printf("[%lu] [GAC] Failed to open directory: %s\n", millis(), path);
    return;
  }

  dir.rewindDirectory();

  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));

    // Skip hidden files and system directories
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    // Build full path
    String fullPath = String(path);
    if (!fullPath.endsWith("/")) {
      fullPath += "/";
    }
    fullPath += name;

    if (file.isDirectory()) {
      // Recursively scan subdirectory
      scanDirectoryRecursive(fullPath.c_str(), depth + 1);
    } else if (StringUtils::checkFileExtension(String(name), ".epub")) {
      // Found an EPUB file
      epubFiles.push_back(fullPath);
      Serial.printf("[%lu] [GAC] Found EPUB: %s\n", millis(), fullPath.c_str());
    }

    file.close();
  }

  dir.close();
}

void GenerateAllCoversActivity::generateCoversForBook(const String& epubPath) {
  Serial.printf("[%lu] [GAC] Processing: %s\n", millis(), epubPath.c_str());

  currentBookTitle = truncateFilename(epubPath, 40);

  // Track memory
  const int heapBefore = ESP.getFreeHeap();

  {
    // Scope to ensure Epub is destroyed and memory freed
    Epub epub(epubPath.c_str(), "/.crosspoint");

    // Load metadata (buildIfMissing=true to create cache structure, skipCss=true saves memory)
    if (!epub.load(true, true)) {
      Serial.printf("[%lu] [GAC] Failed to load EPUB: %s\n", millis(), epubPath.c_str());
      failed++;
      return;
    }

    // Ensure cache directory exists before generating covers
    epub.setupCacheDir();

    // Check what already exists
    const bool coverExists = Storage.exists(epub.getCoverBmpPath().c_str());
    const bool thumb400Exists = Storage.exists(epub.getThumbBmpPath(400).c_str());
    const bool thumb226Exists = Storage.exists(epub.getThumbBmpPath(226).c_str());

    bool bookHadError = false;

    // Generate cover.bmp if missing
    if (!coverExists) {
      Serial.printf("[%lu] [GAC] Generating cover.bmp\n", millis());
      if (epub.generateCoverBmp(false)) {
        coversGenerated++;
      } else {
        Serial.printf("[%lu] [GAC] Failed to generate cover\n", millis());
        failed++;
        bookHadError = true;
      }
    } else {
      Serial.printf("[%lu] [GAC] cover.bmp already exists\n", millis());
      skipped++;
    }

    // Only generate thumbnails if cover exists or was just created
    if (!bookHadError) {
      // Generate thumb_400.bmp if missing (for Classic theme)
      if (!thumb400Exists) {
        Serial.printf("[%lu] [GAC] Generating thumb_400.bmp\n", millis());
        if (epub.generateThumbBmp(400)) {
          thumbsGenerated++;
        } else {
          Serial.printf("[%lu] [GAC] Failed to generate thumb_400\n", millis());
        }
      } else {
        Serial.printf("[%lu] [GAC] thumb_400.bmp already exists\n", millis());
      }

      // Generate thumb_226.bmp if missing (for Lyra theme)
      if (!thumb226Exists) {
        Serial.printf("[%lu] [GAC] Generating thumb_226.bmp\n", millis());
        if (epub.generateThumbBmp(226)) {
          thumbsGenerated++;
        } else {
          Serial.printf("[%lu] [GAC] Failed to generate thumb_226\n", millis());
        }
      } else {
        Serial.printf("[%lu] [GAC] thumb_226.bmp already exists\n", millis());
      }
    }
  }  // epub goes out of scope here, memory freed

  const int heapAfter = ESP.getFreeHeap();
  Serial.printf("[%lu] [GAC] Heap before: %d, after: %d, diff: %d\n", millis(), heapBefore, heapAfter,
                heapAfter - heapBefore);

  // Small delay to allow cleanup
  delay(10);
}

void GenerateAllCoversActivity::renderProgress() {
  renderer.clearScreen();

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  int y = SCREEN_MARGIN;

  // Title
  renderer.drawCenteredText(UI_12_FONT_ID, y, "Generating Covers & Thumbnails");
  y += LINE_HEIGHT + 10;

  // Current book being processed
  renderer.drawText(UI_10_FONT_ID, SCREEN_MARGIN, y, "Processing:");
  y += LINE_HEIGHT;
  renderer.drawText(UI_10_FONT_ID, SCREEN_MARGIN + 10, y, currentBookTitle.c_str());
  y += LINE_HEIGHT + 10;

  // Progress bar
  const int progressBarWidth = screenWidth - 2 * SCREEN_MARGIN;
  const int progressBarHeight = 20;
  const int progressBarX = SCREEN_MARGIN;
  const int progress = totalBooks > 0 ? (currentIndex * 100) / totalBooks : 0;

  // Draw outline
  renderer.drawRect(progressBarX, y, progressBarWidth, progressBarHeight);

  // Draw filled portion
  const int fillWidth = (progressBarWidth - 4) * progress / 100;
  if (fillWidth > 0) {
    renderer.fillRect(progressBarX + 2, y + 2, fillWidth, progressBarHeight - 4);
  }

  // Draw percentage
  String progressText = String(progress) + "%";
  const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, progressText.c_str());
  renderer.drawText(UI_10_FONT_ID, progressBarX + (progressBarWidth - textWidth) / 2, y + progressBarHeight / 2 + 5,
                    progressText.c_str());
  y += progressBarHeight + 20;

  // Statistics
  String booksText = "Books: " + String(currentIndex) + " / " + String(totalBooks);
  renderer.drawText(UI_10_FONT_ID, SCREEN_MARGIN, y, booksText.c_str());
  y += LINE_HEIGHT;

  String coversText = "Covers: " + String(coversGenerated);
  renderer.drawText(UI_10_FONT_ID, SCREEN_MARGIN, y, coversText.c_str());
  y += LINE_HEIGHT;

  String thumbsText = "Thumbnails: " + String(thumbsGenerated);
  renderer.drawText(UI_10_FONT_ID, SCREEN_MARGIN, y, thumbsText.c_str());
  y += LINE_HEIGHT;

  String skippedText = "Skipped: " + String(skipped);
  renderer.drawText(UI_10_FONT_ID, SCREEN_MARGIN, y, skippedText.c_str());
  y += LINE_HEIGHT;

  String failedText = "Failed: " + String(failed);
  renderer.drawText(UI_10_FONT_ID, SCREEN_MARGIN, y, failedText.c_str());
  y += LINE_HEIGHT + 10;

  // Time estimate
  if (currentIndex > 0) {
    const unsigned long elapsed = millis() - startTime;
    const unsigned long avgTimePerBook = elapsed / currentIndex;
    const unsigned long remaining = avgTimePerBook * (totalBooks - currentIndex);

    String estimateText = "Estimated: " + formatTime(remaining) + " remaining";
    renderer.drawText(UI_10_FONT_ID, SCREEN_MARGIN, y, estimateText.c_str());
    y += LINE_HEIGHT + 10;
  }

  // Button hint
  const auto labels = mappedInput.mapLabels("Cancel", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void GenerateAllCoversActivity::renderSummary() {
  renderer.clearScreen();

  const int screenWidth = renderer.getScreenWidth();
  int y = SCREEN_MARGIN + 20;

  // Title
  const char* title = (currentState == CANCELLED) ? "Generation Cancelled" : "Generation Complete";
  renderer.drawCenteredText(UI_12_FONT_ID, y, title);
  y += LINE_HEIGHT + 20;

  // Summary stats
  String processedText = "Processed " + String(currentIndex) + " / " + String(totalBooks) + " books";
  renderer.drawCenteredText(UI_10_FONT_ID, y, processedText.c_str());
  y += LINE_HEIGHT + 10;

  String coversText = "Covers generated: " + String(coversGenerated);
  renderer.drawText(UI_10_FONT_ID, SCREEN_MARGIN + 40, y, coversText.c_str());
  y += LINE_HEIGHT;

  String thumbsText = "Thumbnails generated: " + String(thumbsGenerated);
  renderer.drawText(UI_10_FONT_ID, SCREEN_MARGIN + 40, y, thumbsText.c_str());
  y += LINE_HEIGHT;

  String skippedText = "Already cached: " + String(skipped);
  renderer.drawText(UI_10_FONT_ID, SCREEN_MARGIN + 40, y, skippedText.c_str());
  y += LINE_HEIGHT;

  if (failed > 0) {
    String failedText = "Failed: " + String(failed);
    renderer.drawText(UI_10_FONT_ID, SCREEN_MARGIN + 40, y, failedText.c_str());
    y += LINE_HEIGHT;
  }

  y += 10;

  // Time elapsed
  const unsigned long elapsed = millis() - startTime;
  String timeText = "Completed in " + formatTime(elapsed);
  renderer.drawCenteredText(UI_10_FONT_ID, y, timeText.c_str());

  // Button hint
  const auto labels = mappedInput.mapLabels("Done", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

String GenerateAllCoversActivity::formatTime(unsigned long milliseconds) {
  const unsigned long seconds = milliseconds / 1000;
  const unsigned long minutes = seconds / 60;
  const unsigned long hours = minutes / 60;

  if (hours > 0) {
    return String(hours) + "h " + String(minutes % 60) + "m";
  } else if (minutes > 0) {
    return String(minutes) + "m " + String(seconds % 60) + "s";
  } else {
    return String(seconds) + "s";
  }
}

String GenerateAllCoversActivity::truncateFilename(const String& path, int maxLength) {
  // Extract just the filename from the path
  int lastSlash = path.lastIndexOf('/');
  String filename = (lastSlash >= 0) ? path.substring(lastSlash + 1) : path;

  if (filename.length() <= maxLength) {
    return filename;
  }

  // Truncate with ellipsis
  return filename.substring(0, maxLength - 3) + "...";
}
