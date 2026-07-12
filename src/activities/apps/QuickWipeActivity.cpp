#include "QuickWipeActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_random.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Overwrite-before-delete: fill file with random bytes then remove it.
// Returns number of bytes wiped, or -1 on error.
static int overwriteAndDelete(const char* path, uint32_t fileSize) {
  if (fileSize == 0) {
    Storage.remove(path);
    return 0;
  }

  HalFile file = Storage.open(path, O_WRITE);
  if (!file) {
    Storage.remove(path);
    return 0;
  }

  uint8_t buf[512];
  uint32_t remaining = fileSize;
  int totalWritten = 0;
  while (remaining > 0) {
    uint32_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
    esp_fill_random(buf, chunk);
    int written = static_cast<int>(file.write(buf, chunk));
    if (written <= 0) break;
    totalWritten += written;
    remaining -= static_cast<uint32_t>(written);
  }
  file.flush();
  file.close();

  Storage.remove(path);
  return totalWritten;
}

// Recursively wipe all files in a directory, then remove the (now-empty) directory.
// Returns number of files deleted.
int QuickWipeActivity::wipeDirectory(const char* path) {
  int count = 0;

  HalFile dir = Storage.open(path);
  if (!dir) return 0;

  // Collect entries first to avoid iterator invalidation during deletion.
  // Keep stack usage low: process one entry at a time with a fixed name buffer.
  // We use a two-pass approach: recurse into subdirs inline (openNextFile order is stable
  // on FAT32 even while other entries are deleted, as long as we don't delete the current entry).
  // Safe pattern: iterate, build path, close dir, recurse/delete, reopen — but that is expensive.
  // Instead: iterate entirely first, collect names into small fixed-size batches.

  static constexpr int BATCH = 16;
  static constexpr int NAME_LEN = 256;

  bool done = false;
  while (!done) {
    // Collect up to BATCH entries
    char names[BATCH][NAME_LEN];
    bool isDir[BATCH];
    uint32_t sizes[BATCH];
    int batchCount = 0;

    HalFile entry;
    while (batchCount < BATCH && (entry = dir.openNextFile())) {
      char nameBuf[NAME_LEN];
      entry.getName(nameBuf, sizeof(nameBuf));
      // Skip hidden / system entries
      if (nameBuf[0] == '\0' || nameBuf[0] == '.') {
        entry.close();
        continue;
      }
      // Copy into batch
      snprintf(names[batchCount], NAME_LEN, "%s", nameBuf);
      isDir[batchCount] = entry.isDirectory();
      sizes[batchCount] = isDir[batchCount] ? 0 : static_cast<uint32_t>(entry.size());
      batchCount++;
      entry.close();
    }

    // If we got fewer entries than BATCH, we've exhausted the directory
    if (batchCount < BATCH) done = true;

    dir.close();

    // Process collected entries
    char fullPath[512];
    for (int i = 0; i < batchCount; i++) {
      // Build full path — avoid std::string to minimize heap churn
      int pathLen = static_cast<int>(__builtin_strlen(path));
      if (pathLen > 0 && path[pathLen - 1] == '/') {
        snprintf(fullPath, sizeof(fullPath), "%s%s", path, names[i]);
      } else {
        snprintf(fullPath, sizeof(fullPath), "%s/%s", path, names[i]);
      }

      if (isDir[i]) {
        count += wipeDirectory(fullPath);
      } else {
        overwriteAndDelete(fullPath, sizes[i]);
        count++;
      }
    }

    // Reopen for next batch (only if not done)
    if (!done) {
      dir = Storage.open(path);
      if (!dir) break;
    }
  }

  // Remove the now-empty directory (skip root /biscuit itself — caller decides)
  // We only remove subdirectories; the caller handles the top-level dir
  Storage.rmdir(path);

  return count;
}

// Static wipe — no UI, used by SecurityPinActivity.
int QuickWipeActivity::performWipe() {
  int count = wipeDirectory("/biscuit");
  count += wipeDirectory("/.crosspoint");
  return count;
}

// ---- Activity lifecycle ----

void QuickWipeActivity::onEnter() {
  Activity::onEnter();
  state = CONFIRM;
  confirmStart = 0;
  holdingConfirm = false;
  filesDeleted = 0;
  bytesWiped = 0;
  requestUpdate();
}

void QuickWipeActivity::onExit() {
  Activity::onExit();
}

void QuickWipeActivity::startWipe() {
  state = WIPING;
  requestUpdate();

  // Perform wipe synchronously (blocking) — this runs in the loop() FreeRTOS task.
  // E-ink already shows "Wiping..." from the previous render frame.
  filesDeleted = wipeDirectory("/biscuit");
  filesDeleted += wipeDirectory("/.crosspoint");
  // Recreate directories so the device stays functional
  Storage.mkdir("/biscuit");
  Storage.mkdir("/.crosspoint");

  state = DONE;
  requestUpdate();
}

void QuickWipeActivity::verifyWipe() {
  filesDeleted = 0;  // reuse as "remaining files" counter for verification

  HalFile dir = Storage.open("/biscuit");
  if (dir) {
    HalFile entry;
    while ((entry = dir.openNextFile())) {
      char nameBuf[256];
      entry.getName(nameBuf, sizeof(nameBuf));
      if (nameBuf[0] != '\0' && nameBuf[0] != '.') {
        filesDeleted++;
      }
      entry.close();
    }
    dir.close();
  }

  state = VERIFIED;
  requestUpdate();
}

void QuickWipeActivity::loop() {
  if (state == CONFIRM) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    const bool pressing = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (pressing) {
      if (!holdingConfirm) {
        holdingConfirm = true;
        confirmStart = millis();
        requestUpdate();
      } else {
        // Check held duration
        unsigned long held = millis() - confirmStart;
        // Also accept getHeldTime() if it tracks the current hold
        unsigned long reported = mappedInput.getHeldTime();
        if (reported > held) held = reported;

        if (held >= 3000) {
          holdingConfirm = false;
          startWipe();
          return;
        }
        // Refresh to animate hold progress
        requestUpdate();
      }
    } else {
      if (holdingConfirm) {
        holdingConfirm = false;
        requestUpdate();
      }
    }
    return;
  }

  if (state == WIPING) {
    // Wipe is initiated synchronously from startWipe() which transitions to DONE.
    // Nothing to do here — state transitions happen in startWipe().
    return;
  }

  if (state == DONE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      verifyWipe();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    return;
  }

  if (state == VERIFIED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    return;
  }
}

// ---- Rendering ----

void QuickWipeActivity::render(RenderLock&& lock) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Quick Wipe");

  switch (state) {
    case CONFIRM:   renderConfirm();   break;
    case WIPING:    renderWiping();    break;
    case DONE:      renderDone();      break;
    case VERIFIED:  renderVerified();  break;
  }

  renderer.displayBuffer();
}

void QuickWipeActivity::renderConfirm() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  int y = metrics.topPadding + metrics.headerHeight + 30;

  // Warning title
  renderer.drawCenteredText(UI_12_FONT_ID, y, "! DELETE ALL DATA IN /biscuit/ ?", true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 12;

  renderer.drawCenteredText(UI_10_FONT_ID, y, "All files will be overwritten");
  y += renderer.getLineHeight(UI_10_FONT_ID) + 4;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "and permanently deleted.");
  y += renderer.getLineHeight(UI_10_FONT_ID) + 24;

  // Instruction
  renderer.drawCenteredText(UI_10_FONT_ID, y, "Hold Confirm for 3 seconds to wipe.");
  y += renderer.getLineHeight(UI_10_FONT_ID) + 8;

  // Hold progress bar (only shown while holding)
  if (holdingConfirm) {
    unsigned long held = millis() - confirmStart;
    unsigned long reported = mappedInput.getHeldTime();
    if (reported > held) held = reported;

    constexpr unsigned long HOLD_MS = 3000;
    if (held > HOLD_MS) held = HOLD_MS;

    const int barW = pageWidth - 2 * metrics.contentSidePadding;
    const int barH = 16;
    const int barX = metrics.contentSidePadding;

    renderer.drawRect(barX, y, barW, barH, true);
    int fill = static_cast<int>((held * static_cast<unsigned long>(barW - 2)) / HOLD_MS);
    if (fill > 0) {
      renderer.fillRect(barX + 1, y + 1, fill, barH - 2, true);
    }

    y += barH + 8;
    char pct[16];
    snprintf(pct, sizeof(pct), "%lu%%", (held * 100) / HOLD_MS);
    renderer.drawCenteredText(SMALL_FONT_ID, y, pct);
  }

  // Divider before button hints
  const int divY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - 1;
  renderer.drawLine(metrics.contentSidePadding, divY,
                    pageWidth - metrics.contentSidePadding, divY, true);

  const auto labels = mappedInput.mapLabels("Cancel", "Hold to Wipe", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void QuickWipeActivity::renderWiping() const {
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, "Wiping...", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 20, "Do not power off.");
}

void QuickWipeActivity::renderDone() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  int y = pageHeight / 2 - 50;

  renderer.drawCenteredText(UI_12_FONT_ID, y, "Wipe Complete", true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 16;

  char buf[48];
  snprintf(buf, sizeof(buf), "Files deleted: %d", filesDeleted);
  renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 8;

  renderer.drawCenteredText(SMALL_FONT_ID, y, "Press Confirm to verify.");

  // Bottom divider
  const int divY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - 1;
  renderer.drawLine(metrics.contentSidePadding, divY,
                    pageWidth - metrics.contentSidePadding, divY, true);

  const auto labels = mappedInput.mapLabels("Exit", "Verify", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void QuickWipeActivity::renderVerified() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  int y = pageHeight / 2 - 50;

  if (filesDeleted == 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, "VERIFIED", true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 12;
    renderer.drawCenteredText(UI_10_FONT_ID, y, "0 files remain in /biscuit/");
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, y, "WARNING", true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 12;

    char buf[64];
    snprintf(buf, sizeof(buf), "%d file(s) remain in /biscuit/!", filesDeleted);
    renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
    y += renderer.getLineHeight(UI_10_FONT_ID) + 8;
    renderer.drawCenteredText(SMALL_FONT_ID, y, "Some files could not be deleted.");
  }

  // Bottom divider
  const int divY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - 1;
  renderer.drawLine(metrics.contentSidePadding, divY,
                    pageWidth - metrics.contentSidePadding, divY, true);

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
