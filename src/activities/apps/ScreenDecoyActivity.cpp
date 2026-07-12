#include "ScreenDecoyActivity.h"

#include <HalGPIO.h>
#include <HalPowerManager.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

extern HalPowerManager powerManager;
extern HalGPIO gpio;

// ---------------------------------------------------------------------------
// Lorem ipsum text for the fake reading screen.
// Stored as a single static const to stay in flash.
// ---------------------------------------------------------------------------
static const char LOREM_TEXT[] =
    "The morning light filtered through the heavy curtains, casting long pale "
    "rectangles across the floorboards. Elara had been awake since before dawn, "
    "unable to silence the thoughts that moved through her mind like water finding "
    "the lowest point in a room.\n\n"
    "She rose and walked to the window, pushing the curtain aside with one finger. "
    "The street below was empty save for a delivery cart whose horse stood motionless "
    "in the grey air, breath rising in small clouds. She watched until the driver "
    "reappeared from a doorway and clicked the animal forward, hooves loud on wet stone.\n\n"
    "There was a letter on the desk she had not opened. She knew who it was from and "
    "she knew, with the certainty of someone who has rehearsed a scene many times, "
    "what it would say. Knowledge of that kind was its own kind of weight. She turned "
    "away from it and poured water from the pitcher into the basin, pressing her "
    "hands flat against the cool surface.\n\n"
    "The city was beginning to wake. A bell somewhere counted seven. She had an "
    "appointment at nine and the question of whether she intended to keep it was one "
    "she had been putting off since the previous evening. Putting things off was a "
    "skill she had refined, she thought, almost to an art.";

// ---------------------------------------------------------------------------

const char* ScreenDecoyActivity::decoyName(DecoyType type) {
  switch (type) {
    case FAKE_SHUTDOWN: return "Fake Shutdown";
    case FAKE_ERROR:    return "Fake Error";
    case FAKE_READING:  return "Fake Reading";
    case BLANK:         return "Blank Screen";
    default:            return "Unknown";
  }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ScreenDecoyActivity::onEnter() {
  Activity::onEnter();
  selectedType = FAKE_SHUTDOWN;
  previewMode = true;

  buttonNavigator.onNext([this] {
    selectedType = static_cast<DecoyType>(ButtonNavigator::nextIndex(
        static_cast<int>(selectedType), static_cast<int>(DECOY_COUNT)));
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedType = static_cast<DecoyType>(ButtonNavigator::previousIndex(
        static_cast<int>(selectedType), static_cast<int>(DECOY_COUNT)));
    requestUpdate();
  });

  requestUpdate();
}

void ScreenDecoyActivity::onExit() {
  Activity::onExit();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void ScreenDecoyActivity::loop() {
  if (previewMode) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      previewMode = false;
      requestUpdate();
      return;
    }
    // Left/Right navigate the list
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectedType = static_cast<DecoyType>(ButtonNavigator::previousIndex(
          static_cast<int>(selectedType), static_cast<int>(DECOY_COUNT)));
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectedType = static_cast<DecoyType>(ButtonNavigator::nextIndex(
          static_cast<int>(selectedType), static_cast<int>(DECOY_COUNT)));
      requestUpdate();
      return;
    }
  } else {
    // Preview state — showing what the decoy will look like
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      previewMode = true;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateDecoy();
      // Does not return — device enters deep sleep
    }
  }
}

// ---------------------------------------------------------------------------
// Render dispatch
// ---------------------------------------------------------------------------

void ScreenDecoyActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (previewMode) {
    renderSelection();
  } else {
    renderDecoyPreview();
  }
  renderer.displayBuffer();
}

// ---------------------------------------------------------------------------
// Selection screen
// ---------------------------------------------------------------------------

void ScreenDecoyActivity::renderSelection() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Screen Decoy");

  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

  GUI.drawList(renderer,
               Rect{0, listTop, pageWidth, listBottom - listTop},
               static_cast<int>(DECOY_COUNT),
               static_cast<int>(selectedType),
               [](int i) -> std::string { return decoyName(static_cast<DecoyType>(i)); },
               [](int i) -> std::string {
                 switch (i) {
                   case FAKE_SHUTDOWN: return "Battery critical, shutting down";
                   case FAKE_ERROR:    return "SD card not found error screen";
                   case FAKE_READING:  return "Realistic e-reader book page";
                   case BLANK:         return "White blank screen";
                   default:            return "";
                 }
               });

  const auto labels = mappedInput.mapLabels("Back", "Preview", "Prev", "Next");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---------------------------------------------------------------------------
// Preview screen — show decoy content with a confirmation footer overlay
// ---------------------------------------------------------------------------

void ScreenDecoyActivity::renderDecoyPreview() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw the actual decoy content first so the user sees exactly what will appear
  switch (selectedType) {
    case FAKE_SHUTDOWN: renderFakeShutdown(); break;
    case FAKE_ERROR:    renderFakeError();    break;
    case FAKE_READING:  renderFakeReading();  break;
    case BLANK:         renderBlank();        break;
    default:            break;
  }

  // Overlay a small confirmation bar at the very bottom so the user knows
  // this is still preview mode. Use a solid black band for visibility over
  // any decoy content that might be drawn near the bottom.
  const int barH = metrics.buttonHintsHeight + metrics.verticalSpacing + 4;
  renderer.fillRect(0, pageHeight - barH, pageWidth, barH, true);

  // Text on the black band — inverted (white-on-black = black=false param)
  const int textY = pageHeight - barH + (barH - renderer.getTextHeight(SMALL_FONT_ID)) / 2;
  renderer.drawCenteredText(SMALL_FONT_ID, textY,
                            "PREVIEW  |  Confirm = activate & sleep  |  Back = cancel",
                            false);
}

// ---------------------------------------------------------------------------
// activateDecoy — render final image, display it, enter deep sleep
// ---------------------------------------------------------------------------

void ScreenDecoyActivity::activateDecoy() {
  renderer.clearScreen();

  switch (selectedType) {
    case FAKE_SHUTDOWN: renderFakeShutdown(); break;
    case FAKE_ERROR:    renderFakeError();    break;
    case FAKE_READING:  renderFakeReading();  break;
    case BLANK:         renderBlank();        break;
    default:            break;
  }

  // Full refresh produces the cleanest image for a persistent decoy
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);

  // Deep sleep — e-ink retains the image without power
  powerManager.startDeepSleep(gpio);

  // Unreachable, but keeps the compiler quiet
  while (true) {}
}

// ---------------------------------------------------------------------------
// DECOY: Fake Shutdown
// ---------------------------------------------------------------------------

void ScreenDecoyActivity::renderFakeShutdown() const {
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Centre everything vertically in the lower third — like a real shutdown notice
  const int centerY = pageHeight / 2;

  // Battery icon — a mostly-empty rectangle
  // Body: 60 x 30, terminal nub: 6 x 12
  constexpr int iconW = 60;
  constexpr int iconH = 30;
  constexpr int nubW  = 6;
  constexpr int nubH  = 12;

  const int iconX = pageWidth / 2 - iconW / 2;
  const int iconY = centerY - 80;

  // Outer body
  renderer.drawRect(iconX, iconY, iconW, iconH, true);
  // Terminal nub (right side)
  renderer.fillRect(iconX + iconW, iconY + (iconH - nubH) / 2, nubW, nubH, true);
  // 1% fill — just a thin 2px sliver at the left
  renderer.fillRect(iconX + 2, iconY + 2, 2, iconH - 4, true);

  // "1%" label inside the icon, centred
  const int pctTW = renderer.getTextWidth(SMALL_FONT_ID, "1%");
  renderer.drawText(SMALL_FONT_ID,
                    iconX + (iconW - pctTW) / 2,
                    iconY + (iconH - renderer.getTextHeight(SMALL_FONT_ID)) / 2,
                    "1%", true);

  // "Battery Critical" heading
  int y = iconY + iconH + 20;
  renderer.drawCenteredText(UI_12_FONT_ID, y, "Battery Critical", true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 10;

  renderer.drawCenteredText(UI_10_FONT_ID, y, "Shutting down...");
  y += renderer.getLineHeight(UI_10_FONT_ID) + 6;

  renderer.drawCenteredText(SMALL_FONT_ID, y, "Please charge your device.");
}

// ---------------------------------------------------------------------------
// DECOY: Fake Error
// ---------------------------------------------------------------------------

void ScreenDecoyActivity::renderFakeError() const {
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics   = UITheme::getInstance().getMetrics();

  // Draw a thick header band to mimic an OS error screen
  constexpr int bandH = 56;
  renderer.fillRect(0, 0, pageWidth, bandH, true);

  // "System Error" in inverted white-on-black
  const int titleTW = renderer.getTextWidth(UI_12_FONT_ID, "System Error", EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID,
                    (pageWidth - titleTW) / 2,
                    (bandH - renderer.getTextHeight(UI_12_FONT_ID)) / 2,
                    "System Error", false, EpdFontFamily::BOLD);

  int y = bandH + 30;

  renderer.drawCenteredText(UI_12_FONT_ID, y, "SD Card Not Found", true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 14;

  // Horizontal rule
  renderer.drawLine(metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding, y, true);
  y += 14;

  renderer.drawCenteredText(UI_10_FONT_ID, y, "Please insert a valid SD card and restart.");
  y += renderer.getLineHeight(UI_10_FONT_ID) + 10;

  renderer.drawCenteredText(UI_10_FONT_ID, y, "No storage device detected on bus SPI1.");
  y += renderer.getLineHeight(UI_10_FONT_ID) + 20;

  // Error code in monospace style (SMALL_FONT_ID is a narrow font)
  renderer.drawCenteredText(SMALL_FONT_ID, y, "Error code: 0xE0040002");
  y += renderer.getLineHeight(SMALL_FONT_ID) + 6;

  renderer.drawCenteredText(SMALL_FONT_ID, y, "Module: HalStorage  |  Line: 0x0094");
  y += renderer.getLineHeight(SMALL_FONT_ID) + 20;

  // Separator
  renderer.drawLine(metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding, y, true);
  y += 14;

  renderer.drawCenteredText(SMALL_FONT_ID, y, "Contact support at crosspoint.dev/help");
}

// ---------------------------------------------------------------------------
// DECOY: Fake Reading — realistic e-reader book page, no chrome
// ---------------------------------------------------------------------------

void ScreenDecoyActivity::renderFakeReading() const {
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Generous margins matching typical e-reader layout
  constexpr int marginH = 36;   // left/right
  constexpr int marginTop = 40;
  constexpr int marginBottom = 50;  // leaves room for page number

  const int textWidth = pageWidth - marginH * 2;
  const int maxLines  = (pageHeight - marginTop - marginBottom) / renderer.getLineHeight(UI_10_FONT_ID);

  // Word-wrap the lorem text to fit the page
  auto lines = renderer.wrappedText(UI_10_FONT_ID, LOREM_TEXT, textWidth, maxLines);

  int y = marginTop;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);

  for (const auto& line : lines) {
    renderer.drawText(UI_10_FONT_ID, marginH, y, line.c_str());
    y += lineH;
    if (y + lineH > pageHeight - marginBottom) break;
  }

  // Page number at the bottom — centred, small
  const int pgY = pageHeight - marginBottom + 10;
  renderer.drawCenteredText(SMALL_FONT_ID, pgY, "Page 127 of 342");
}

// ---------------------------------------------------------------------------
// DECOY: Blank — white screen (e-ink default after clearScreen)
// ---------------------------------------------------------------------------

void ScreenDecoyActivity::renderBlank() const {
  // clearScreen() already called by caller — nothing more needed
  (void)renderer;
}
