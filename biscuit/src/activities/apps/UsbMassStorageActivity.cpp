#include "UsbMassStorageActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void UsbMassStorageActivity::onEnter() {
  Activity::onEnter();
  state = READY;
  requestUpdate();
}

void UsbMassStorageActivity::loop() {
  switch (state) {
    case READY:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        finish();
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        initMassStorage();
        state = ACTIVE;
        requestUpdate();
      }
      break;

    case ACTIVE:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        deinitMassStorage();
        state = EJECTED;
        requestUpdate();
      }
      break;

    case EJECTED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        finish();
        return;
      }
      break;
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void UsbMassStorageActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int headerBottom = metrics.topPadding + metrics.headerHeight;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "USB Storage");

  switch (state) {
    // ------------------------------------------------------------------
    case READY: {
      int y = headerBottom + 60;

      renderer.drawCenteredText(UI_12_FONT_ID, y,
                                "Share SD card as USB drive",
                                true, EpdFontFamily::BOLD);
      y += 50;

      renderer.drawCenteredText(UI_10_FONT_ID, y,
                                "Connect USB-C cable to PC,");
      y += 30;
      renderer.drawCenteredText(UI_10_FONT_ID, y,
                                "then press OK to enable mass storage.");
      y += 55;

      // Warning box
      const int warnPad = 16;
      const int warnW   = pageWidth - warnPad * 2;
      const int warnH   = 58;
      renderer.drawRect(warnPad, y, warnW, warnH, true);
      renderer.drawCenteredText(SMALL_FONT_ID, y + 12,
                                "Warning: SD card will be unavailable");
      renderer.drawCenteredText(SMALL_FONT_ID, y + 30,
                                "to firmware while active.");

      const auto labels = mappedInput.mapLabels("Back", "Enable", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }

    // ------------------------------------------------------------------
    case ACTIVE: {
      // Simple stylised USB icon — rectangular body + connector tab
      const int iconW  = 80;
      const int iconH  = 110;
      const int iconX  = pageWidth  / 2 - iconW / 2;
      const int iconY  = headerBottom + 40;

      // Main body (white fill, black border)
      renderer.fillRect(iconX, iconY, iconW, iconH, false);
      renderer.drawRect(iconX, iconY, iconW, iconH, true);

      // Connector tab at the bottom centre
      const int tabW = 30;
      const int tabH = 20;
      const int tabX = pageWidth / 2 - tabW / 2;
      const int tabY = iconY + iconH;
      renderer.fillRect(tabX, tabY, tabW, tabH, false);
      renderer.drawRect(tabX, tabY, tabW, tabH, true);

      // Two small contact rectangles inside the tab
      const int contactW = 8;
      const int contactH = 10;
      renderer.fillRect(tabX + 5,          tabY + 5, contactW, contactH, true);
      renderer.fillRect(tabX + tabW - 5 - contactW, tabY + 5, contactW, contactH, true);

      // Horizontal line across body middle (decorative)
      renderer.fillRect(iconX + 8, iconY + iconH / 2 - 1, iconW - 16, 2, true);

      int y = iconY + iconH + tabH + 30;
      renderer.drawCenteredText(UI_12_FONT_ID, y,
                                "USB Storage Active",
                                true, EpdFontFamily::BOLD);
      y += 40;
      renderer.drawCenteredText(UI_10_FONT_ID, y,
                                "PC can now read/write the SD card.");
      y += 30;
      renderer.drawCenteredText(UI_10_FONT_ID, y,
                                "Press Back to safely disconnect.");

      const auto labels = mappedInput.mapLabels("Disconnect", "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }

    // ------------------------------------------------------------------
    case EJECTED: {
      int y = pageHeight / 2 - 40;

      renderer.drawCenteredText(UI_12_FONT_ID, y,
                                "Safely disconnected.",
                                true, EpdFontFamily::BOLD);
      y += 50;
      renderer.drawCenteredText(UI_10_FONT_ID, y,
                                "SD card restored for firmware use.");

      const auto labels = mappedInput.mapLabels("Back", "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
  }

  renderer.displayBuffer();
}

// ---------------------------------------------------------------------------
// USB MSC stubs — require manual TinyUSB integration
// ---------------------------------------------------------------------------

void UsbMassStorageActivity::initMassStorage() {
  // TODO: MANUAL — implement TinyUSB MSC descriptor
  // Steps:
  // 1. Unmount SD card from SdFat (Storage.end() or similar)
  // 2. Initialize TinyUSB MSC class with SD card sector read/write callbacks
  // 3. Switch USB from CDC-only to CDC+MSC composite device
  // 4. tud_msc_set_callback() for read10, write10, inquiry, etc.
}

void UsbMassStorageActivity::deinitMassStorage() {
  // TODO: MANUAL — restore CDC-only mode
  // Steps:
  // 1. Flush any pending writes
  // 2. Deinit TinyUSB MSC
  // 3. Re-mount SD card for firmware use (Storage.begin() or similar)
}
