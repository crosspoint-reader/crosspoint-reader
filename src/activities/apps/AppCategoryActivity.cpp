#include "AppCategoryActivity.h"

#include <I18n.h>
#include <HalStorage.h>
#include <cctype>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

void AppCategoryActivity::onEnter() {
  Activity::onEnter();
  backPressedHere = false;

  tileIndices.clear();
  for (int i = 0; i < (int)entries.size(); i++) {
    if (!entries[i].isSectionHeader) tileIndices.push_back(i);
  }
  selectorIndex = 0;
  scrollRow = 0;

  if (requiresDisclaimer && !RADIO.isDisclaimerAcknowledged()) {
    disclaimerShown = true;
  }

  requestUpdate();
}

void AppCategoryActivity::launchSelected() {
  if (selectorIndex < 0 || selectorIndex >= (int)tileIndices.size()) return;
  int entryIdx = tileIndices[selectorIndex];

  if (categoryIndex >= 0) {
    char path[40];
    snprintf(path, sizeof(path), "/shortbread/lastused_%d.txt", categoryIndex);
    FsFile file;
    if (Storage.openFileForWrite("APPS", path, file)) {
      file.write((const uint8_t*)entries[entryIdx].nameStrId, strlen(entries[entryIdx].nameStrId));
      file.close();
    }
  }

  auto app = entries[entryIdx].factory(renderer, mappedInput);
  if (app) activityManager.pushActivity(std::move(app));
}

void AppCategoryActivity::loop() {
  if (disclaimerShown) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      RADIO.setDisclaimerAcknowledged();
      disclaimerShown = false;
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  const int tileCount = (int)tileIndices.size();
  if (tileCount == 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }

  // Right: next tile (wraps row)
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (selectorIndex + 1 < tileCount) {
      selectorIndex++;
      requestUpdate();
    }
  }

  // Left: prev tile
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (selectorIndex > 0) {
      selectorIndex--;
      requestUpdate();
    }
  }

  // Down: short press = one row, long press = full page
  {
    const unsigned long held = mappedInput.getHeldTime();
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      int step = (held >= 600) ? TILE_COLS * TILE_ROWS_VISIBLE : TILE_COLS;
      int next = selectorIndex + step;
      selectorIndex = (next < tileCount) ? next : tileCount - 1;
      requestUpdate();
    }
  }

  // Up: short press = one row, long press = full page
  {
    const unsigned long held = mappedInput.getHeldTime();
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      int step = (held >= 600) ? TILE_COLS * TILE_ROWS_VISIBLE : TILE_COLS;
      int prev = selectorIndex - step;
      selectorIndex = (prev >= 0) ? prev : 0;
      requestUpdate();
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    launchSelected();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    backPressedHere = true;
  }

  const unsigned long heldTime = mappedInput.getHeldTime();
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!backPressedHere) return;
    backPressedHere = false;
    if (heldTime >= 2500) {
      onGoHome();
    } else {
      finish();
    }
  }
}

static void drawWrapped(GfxRenderer& r, int fontId, int x, int y, int maxW, int lineH,
                        const char* text, bool black, EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  std::string word;
  std::string line;
  int curY = y;
  const char* p = text;

  auto flush = [&]() {
    if (!line.empty()) {
      r.drawText(fontId, x, curY, line.c_str(), black, style);
      curY += lineH + 2;
      line.clear();
    }
  };

  while (true) {
    char c = *p++;
    if (c == ' ' || c == '\0') {
      if (!word.empty()) {
        std::string test = line.empty() ? word : line + " " + word;
        if (r.getTextWidth(fontId, test.c_str(), style) > maxW) {
          flush();
          line = word;
        } else {
          line = test;
        }
        word.clear();
      }
      if (c == '\0') break;
    } else {
      word += c;
    }
  }
  flush();
}

void AppCategoryActivity::drawAppTile(int entryIdx, int x, int y, int w, int h, bool selected) const {
  if (selected) {
    renderer.fillRect(x, y, w, h, true);
  } else {
    renderer.drawRect(x, y, w, h, true);
  }

  constexpr int pad = 10;
  const int maxTextW = w - pad * 2;
  const int nameLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int descLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const char* name = entries[entryIdx].nameStrId;
  const char* desc = entries[entryIdx].description;

  // Uppercase the name
  std::string nameUpper;
  for (const char* p = name; *p; p++) nameUpper += (char)toupper((unsigned char)*p);

  int nameY = y + pad;
  drawWrapped(renderer, UI_10_FONT_ID, x + pad, nameY, maxTextW, nameLineH, nameUpper.c_str(), !selected, EpdFontFamily::BOLD);

  if (desc && desc[0] != '\0') {
    int descY = nameY + nameLineH + 2 + 4;
    drawWrapped(renderer, SMALL_FONT_ID, x + pad, descY, maxTextW, descLineH, desc, !selected);
  }

  if (entries[entryIdx].hasActiveState && entries[entryIdx].hasActiveState()) {
    int dotX = x + w - pad - 6;
    int dotY = y + pad + 2;
    renderer.fillRect(dotX, dotY, 6, 6, !selected);
  }
}

void AppCategoryActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const int headerY = metrics.topPadding;

  if (disclaimerShown) {
    GUI.drawHeader(renderer, Rect{0, headerY, pageWidth, metrics.headerHeight}, title);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DISCLAIMER));
    const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int tileCount = (int)tileIndices.size();
  char countStr[16];
  snprintf(countStr, sizeof(countStr), "%d items", tileCount);
  GUI.drawHeader(renderer, Rect{0, headerY, pageWidth, metrics.headerHeight}, title, countStr);

  const int gridTop = headerY + metrics.headerHeight + metrics.verticalSpacing;
  const int gridBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int gridHeight = gridBottom - gridTop;

  constexpr int sidePad = 14;
  const int tileW = (pageWidth - sidePad * 2 - TILE_GAP * (TILE_COLS - 1)) / TILE_COLS;
  const int tileH = (gridHeight - TILE_GAP * (TILE_ROWS_VISIBLE - 1)) / TILE_ROWS_VISIBLE;
  const int rowH = tileH + TILE_GAP;

  // Keep selected tile in view
  const int currentRow = selectorIndex / TILE_COLS;
  if (currentRow < scrollRow) scrollRow = currentRow;
  if (currentRow >= scrollRow + TILE_ROWS_VISIBLE) scrollRow = currentRow - TILE_ROWS_VISIBLE + 1;

  for (int i = 0; i < tileCount; i++) {
    int row = i / TILE_COLS;
    int col = i % TILE_COLS;
    if (row < scrollRow || row >= scrollRow + TILE_ROWS_VISIBLE) continue;

    int x = sidePad + col * (tileW + TILE_GAP);
    int y = gridTop + (row - scrollRow) * rowH;
    drawAppTile(tileIndices[i], x, y, tileW, tileH, i == selectorIndex);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, "^", "v");

  renderer.displayBuffer();
}
