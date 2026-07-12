// test/test_preview/test_preview.cpp
// ============================================================
// biscuit. Screen Preview Tool
//
// Renders activity screens to BMP files without hardware.
// After running: open test/preview_*.bmp in any image viewer.
//
// Usage:
//   pio test -e native -f test_preview
//
// To preview YOUR new activity:
//   1. Copy an existing render_xxx() function below
//   2. Replace the drawing calls with your activity's render() logic
//   3. Add a RUN_TEST line in main()
//   4. Run and open the BMP
// ============================================================

#include <unity.h>
#include "BitmapRenderer.h"

// We use BitmapRenderer (a real pixel-drawing GfxRenderer) instead of the no-op mock
static GfxRenderer renderer;

// Font IDs matching the firmware
#define UI_10_FONT_ID 10
#define UI_12_FONT_ID 12
#define SMALL_FONT_ID 8

// ============================================================
// Helper: mimics GUI.drawHeader / GUI.drawButtonHints
// ============================================================
static void drawHeader(const char* title, const char* subtitle = nullptr) {
  renderer.drawCenteredText(UI_12_FONT_ID, 12, title, true, 1);
  renderer.drawLine(15, 42, 465, 42);
  if (subtitle) renderer.drawCenteredText(SMALL_FONT_ID, 48, subtitle);
}

static void drawButtonHints(const char* b1, const char* b2, const char* b3, const char* b4) {
  renderer.drawLine(0, 768, 480, 768);
  if (b1) renderer.drawText(SMALL_FONT_ID, 15, 775, b1);
  if (b2) renderer.drawCenteredText(SMALL_FONT_ID, 775, b2);
  if (b4) {
    int w = renderer.getTextWidth(SMALL_FONT_ID, b4);
    renderer.drawText(SMALL_FONT_ID, 465 - w, 775, b4);
  }
}

static void drawListItem(int y, const char* text, bool selected) {
  if (selected) {
    renderer.fillRect(0, y, 480, 36, true);
    renderer.drawText(UI_10_FONT_ID, 20, y + 8, text, false);
  } else {
    renderer.drawText(UI_10_FONT_ID, 20, y + 8, text, true);
  }
}

static void drawPip(int cx, int cy, int r) {
  for (int dy = -r; dy <= r; dy++) {
    int dx = 0;
    while ((dx+1)*(dx+1) + dy*dy <= r*r) dx++;
    renderer.fillRect(cx - dx, cy + dy, dx * 2 + 1, 1, true);
  }
}

static void drawDie(int x, int y, int size, int value) {
  renderer.fillRect(x + 4, y + 4, size, size, true);    // shadow
  renderer.fillRect(x, y, size, size, false);             // white face
  renderer.drawRect(x, y, size, size);                    // outer border
  renderer.drawRect(x + 2, y + 2, size - 4, size - 4);   // inner border

  int pip = size / 8;
  int m = size / 4;
  int l = x + m, r = x + size - m;
  int t = y + m, b = y + size - m;
  int mx = x + size/2, my = y + size/2;

  switch (value) {
    case 1: drawPip(mx,my,pip); break;
    case 2: drawPip(r,t,pip); drawPip(l,b,pip); break;
    case 3: drawPip(r,t,pip); drawPip(mx,my,pip); drawPip(l,b,pip); break;
    case 4: drawPip(l,t,pip); drawPip(r,t,pip); drawPip(l,b,pip); drawPip(r,b,pip); break;
    case 5: drawPip(l,t,pip); drawPip(r,t,pip); drawPip(mx,my,pip); drawPip(l,b,pip); drawPip(r,b,pip); break;
    case 6: drawPip(l,t,pip); drawPip(r,t,pip); drawPip(l,my,pip); drawPip(r,my,pip); drawPip(l,b,pip); drawPip(r,b,pip); break;
  }
}

// ============================================================
// Screen renders — copy & modify for your new activity
// ============================================================

void render_dice_2d6() {
  renderer.clearScreen();
  drawHeader("Dice Roller");

  // Two d6 dice thrown on table
  drawDie(75, 180, 120, 5);
  drawDie(285, 210, 120, 3);

  renderer.drawCenteredText(UI_10_FONT_ID, 420, "2d6: 5 + 3 = 8", true, 1);
  drawButtonHints("Back", "Reroll", "", "");

  TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_dice_2d6.bmp"));
}

void render_dice_4d6() {
  renderer.clearScreen();
  drawHeader("Dice Roller");

  drawDie(55, 120, 100, 6);
  drawDie(265, 135, 100, 2);
  drawDie(65, 310, 100, 4);
  drawDie(275, 320, 100, 1);

  renderer.drawCenteredText(UI_10_FONT_ID, 500, "4d6: 6 + 2 + 4 + 1 = 13", true, 1);
  drawButtonHints("Back", "Reroll", "", "");

  TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_dice_4d6.bmp"));
}

void render_dice_1d20() {
  renderer.clearScreen();
  drawHeader("Dice Roller");

  // Single d20 — large, centered, number inside
  int x = 170, y = 220, size = 140;
  renderer.fillRect(x+4, y+4, size, size, true);
  renderer.fillRect(x, y, size, size, false);
  renderer.drawRect(x, y, size, size);
  renderer.drawRect(x+2, y+2, size-4, size-4);
  renderer.drawCenteredText(UI_12_FONT_ID, y + size/2 - 12, "17", true, 1);
  renderer.drawCenteredText(SMALL_FONT_ID, y + size - 20, "d20");

  renderer.drawCenteredText(UI_10_FONT_ID, 440, "d20: 17", true, 1);
  drawButtonHints("Back", "Reroll", "", "");

  TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_dice_d20.bmp"));
}

void render_beacon_select() {
  renderer.clearScreen();
  drawHeader("Beacon Spam");

  const char* modes[] = {"Random", "Custom (SD)", "Rickroll", "Funny"};
  for (int i = 0; i < 4; i++) {
    drawListItem(70 + i * 38, modes[i], i == 2);
  }
  drawButtonHints("Back", "Select", "Up", "Down");

  TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_beacon_select.bmp"));
}

void render_beacon_running() {
  renderer.clearScreen();
  drawHeader("Beacon Spam");

  renderer.drawCenteredText(UI_10_FONT_ID, 300, "RICKROLL");
  renderer.drawCenteredText(UI_12_FONT_ID, 340, "Never Gonna Give You Up", true, 1);
  renderer.drawCenteredText(UI_10_FONT_ID, 390, "SSID 1/8  Cycle 12");
  renderer.drawCenteredText(UI_10_FONT_ID, 420, "Interval: 2000ms");
  drawButtonHints("Stop", "", "", "");

  TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_beacon_running.bmp"));
}

void render_evil_portal() {
  renderer.clearScreen();
  drawHeader("Evil Portal");

  int y = 100;
  renderer.drawText(SMALL_FONT_ID, 15, y, "ACTIVE", true, 1); y += 45;
  renderer.drawText(SMALL_FONT_ID, 15, y, "SSID:", true, 1);
  renderer.drawText(UI_10_FONT_ID, 80, y, "Free WiFi"); y += 45;
  renderer.drawText(UI_10_FONT_ID, 15, y, "Clients: 3"); y += 45;
  renderer.drawText(UI_10_FONT_ID, 15, y, "Captured: 2", true, 1); y += 45;
  renderer.drawText(UI_10_FONT_ID, 15, y, "Last: john@example.com");
  drawButtonHints("Stop", "", "", "");

  TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_evil_portal.bmp"));
}

void render_apps_menu() {
  renderer.clearScreen();
  drawHeader("Apps");

  const char* cats[] = {"Network Tools", "Wireless Testing", "Games", "Utilities"};
  for (int i = 0; i < 4; i++) {
    drawListItem(70 + i * 38, cats[i], i == 1);
  }
  drawButtonHints("Back", "Select", "Up", "Down");

  TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_apps_menu.bmp"));
}



void render_countdowntimer() {

  renderer.clearScreen();
  drawHeader("CountdownTimer");

  // === YOUR RENDER CODE HERE ===
  // Copy drawing calls from your activity's render() method.
  // Available functions:
  //   renderer.drawCenteredText(UI_12_FONT_ID, y, "text", true, 1);  // bold
  //   renderer.drawCenteredText(UI_10_FONT_ID, y, "text");            // normal
  //   renderer.drawText(UI_10_FONT_ID, x, y, "text");
  //   renderer.drawText(SMALL_FONT_ID, x, y, "small text");
  //   renderer.fillRect(x, y, w, h, true);    // black
  //   renderer.fillRect(x, y, w, h, false);   // white
  //   renderer.drawRect(x, y, w, h);           // border
  //   renderer.drawLine(x1, y1, x2, y2);
  //   drawListItem(y, "Item", selected);       // list row
  //   drawDie(x, y, size, value);              // d6 die face
  //   drawHeader("Title", "subtitle");
  //
  // Screen: 480 wide x 800 tall
  // Header ends at y=42, button hints start at y=768

  renderer.drawCenteredText(UI_12_FONT_ID, 380, "TODO: add render code", true, 1);

  drawButtonHints("Back", "Select", "Up", "Down");

  TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_countdowntimer.bmp"));
}


void render_diceroller() {
  // Reference: src\activities\apps\DiceRollerActivity.cpp
  renderer.clearScreen();
  drawHeader("DiceRoller");

  // === YOUR RENDER CODE HERE ===
  // Copy drawing calls from your activity's render() method.
  // Available functions:
  //   renderer.drawCenteredText(UI_12_FONT_ID, y, "text", true, 1);  // bold
  //   renderer.drawCenteredText(UI_10_FONT_ID, y, "text");            // normal
  //   renderer.drawText(UI_10_FONT_ID, x, y, "text");
  //   renderer.drawText(SMALL_FONT_ID, x, y, "small text");
  //   renderer.fillRect(x, y, w, h, true);    // black
  //   renderer.fillRect(x, y, w, h, false);   // white
  //   renderer.drawRect(x, y, w, h);           // border
  //   renderer.drawLine(x1, y1, x2, y2);
  //   drawListItem(y, "Item", selected);       // list row
  //   drawDie(x, y, size, value);              // d6 die face
  //   drawHeader("Title", "subtitle");
  //
  // Screen: 480 wide x 800 tall
  // Header ends at y=42, button hints start at y=768

  renderer.drawCenteredText(UI_12_FONT_ID, 380, "TODO: add render code", true, 1);

  drawButtonHints("Back", "Select", "Up", "Down");

  TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_diceroller.bmp"));
}
// ============================================================
// YOUR NEW ACTIVITY — copy this template:
// ============================================================
//
// void render_my_new_activity() {
//   renderer.clearScreen();
//   drawHeader("My Activity");
//
//   // ... your drawing code here ...
//   // Use the same calls as in your real render() method:
//   //   renderer.drawCenteredText(...)
//   //   renderer.drawText(...)
//   //   renderer.fillRect(...)
//   //   renderer.drawRect(...)
//   //   renderer.drawLine(...)
//   //   drawListItem(y, "text", selected)
//
//   drawButtonHints("Back", "Select", "Up", "Down");
//
//   TEST_ASSERT_TRUE(renderer.saveBMP("test/preview_my_activity.bmp"));
// }

// ============================================================
void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(render_dice_2d6);
  RUN_TEST(render_dice_4d6);
  RUN_TEST(render_dice_1d20);
  RUN_TEST(render_beacon_select);
  RUN_TEST(render_beacon_running);
  RUN_TEST(render_evil_portal);
  RUN_TEST(render_apps_menu);
  // RUN_TEST(render_my_new_activity);  // ← uncomment when ready
    RUN_TEST(render_countdowntimer);
    RUN_TEST(render_diceroller);
  return UNITY_END();
}
