#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <cstring>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ReadingStatsActivity::scanLibrary() {
  totalBooks = 0;
  booksWithProgress = 0;
  storageBytesUsed = 0;

  HalFile cpDir = Storage.open("/.crosspoint");
  if (!cpDir) return;

  HalFile entry;
  while ((entry = cpDir.openNextFile())) {
    char nameBuf[64];
    entry.getName(nameBuf, sizeof(nameBuf));
    std::string name = nameBuf;

    if (!entry.isDirectory() || name.rfind("epub_", 0) != 0) {
      entry.close();
      continue;
    }
    totalBooks++;

    // Check for progress.bin inside this subdirectory
    std::string progressPath = std::string("/.crosspoint/") + name + "/progress.bin";
    if (Storage.exists(progressPath.c_str())) {
      booksWithProgress++;
      HalFile pf = Storage.open(progressPath.c_str());
      if (pf) { storageBytesUsed += pf.fileSize(); pf.close(); }
    }
    entry.close();
  }
  cpDir.close();
}

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  scanLibrary();

  const std::string& ep = APP_STATE.openEpubPath;
  if (!ep.empty()) {
    size_t slash = ep.rfind('/');
    currentBook = (slash != std::string::npos) ? ep.substr(slash + 1) : ep;
  } else {
    currentBook = "None";
  }

  requestUpdate();
}

void ReadingStatsActivity::onExit() { Activity::onExit(); }

void ReadingStatsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { finish(); return; }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    // Rescan
    scanLibrary();
    requestUpdate();
  }
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Reading Stats");

  const int top = metrics.topPadding + metrics.headerHeight + 30;
  const int lineH = 50;

  renderer.drawCenteredText(UI_12_FONT_ID, top, "Library", true, EpdFontFamily::BOLD);

  char buf[64];
  snprintf(buf, sizeof(buf), "Books: %d", totalBooks);
  renderer.drawCenteredText(UI_10_FONT_ID, top + lineH, buf);

  snprintf(buf, sizeof(buf), "With progress: %d", booksWithProgress);
  renderer.drawCenteredText(UI_10_FONT_ID, top + lineH * 2, buf);

  // Storage used in KB
  snprintf(buf, sizeof(buf), "Progress data: %llu B", (unsigned long long)storageBytesUsed);
  renderer.drawCenteredText(UI_10_FONT_ID, top + lineH * 3, buf);

  renderer.drawCenteredText(UI_12_FONT_ID, top + lineH * 4 + 10, "Now Reading", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, top + lineH * 5 + 10, currentBook.c_str());

  const auto labels = mappedInput.mapLabels("Back", "Rescan", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
