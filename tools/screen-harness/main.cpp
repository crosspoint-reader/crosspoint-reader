#include <EpdFont.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <SPI.h>
#include <builtinFonts/all.h>

#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "activities/boot_sleep/BootActivity.h"
#include "fontIds.h"

namespace {

void installFonts(GfxRenderer& renderer) {
  static EpdFont smallFont(&notosans_8_regular);
  static EpdFontFamily smallFontFamily(&smallFont);

  static EpdFont ui10RegularFont(&ubuntu_10_regular);
  static EpdFont ui10BoldFont(&ubuntu_10_bold);
  static EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

  static EpdFont ui12RegularFont(&ubuntu_12_regular);
  static EpdFont ui12BoldFont(&ubuntu_12_bold);
  static EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

  static EpdFont bookerly14RegularFont(&bookerly_14_regular);
  static EpdFont bookerly14BoldFont(&bookerly_14_bold);
  static EpdFont bookerly14ItalicFont(&bookerly_14_italic);
  static EpdFont bookerly14BoldItalicFont(&bookerly_14_bolditalic);
  static EpdFontFamily bookerly14FontFamily(&bookerly14RegularFont, &bookerly14BoldFont, &bookerly14ItalicFont,
                                            &bookerly14BoldItalicFont);

  renderer.insertFontFamily(SMALL_FONT_ID, &smallFontFamily);
  renderer.insertFontFamily(UI_10_FONT_ID, &ui10FontFamily);
  renderer.insertFontFamily(UI_12_FONT_ID, &ui12FontFamily);
  renderer.insertFontFamily(BOOKERLY_14_FONT_ID, &bookerly14FontFamily);
}

void saveSnapshot(HalDisplay& display, const std::filesystem::path& outDir, const std::string& name) {
  const auto outputPath = outDir / (name + ".pbm");
  const auto outputPathStr = outputPath.string();
  display.saveFrameBufferAsPBM(outputPathStr.c_str());
  std::cout << "wrote " << outputPathStr << '\n';
}

void drawHeader(GfxRenderer& renderer, const char* title) {
  const int right = renderer.getScreenWidth() - 18;
  renderer.drawText(UI_12_FONT_ID, 18, 14, title, true, EpdFontFamily::BOLD);
  renderer.drawLine(18, 40, right, 40);
}

void drawBoot(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  BootActivity boot(renderer, mappedInput);
  boot.onEnter();
}

void drawHomeMock(GfxRenderer& renderer) {
  renderer.clearScreen();
  drawHeader(renderer, "Home");

  // ForkDrift layout: 3×2 book-cover grid + 2×2 button menu.
  // Mirrors ForkDriftTheme geometry: contentSidePadding=20, hPadding=8,
  // homeCoverHeight=120, homeCoverTileHeight=380 (2 rows × 190 px each).
  constexpr int pad = 20;
  constexpr int hPad = 8;
  constexpr int tileW = (480 - 2 * pad) / 3;  // 146 px per column
  constexpr int tileH = 190;                  // singleRowH = homeCoverTileHeight / 2
  constexpr int coverH = 120;
  constexpr int gridStartY = 56;

  struct Book {
    const char* title;
    const char* author;
  };
  static constexpr Book books[6] = {
      {"Left Hand of Darkness", "Le Guin"}, {"Dune", "Frank Herbert"},        {"Foundation", "Isaac Asimov"},
      {"Neuromancer", "W. Gibson"},         {"Name of the Wind", "Rothfuss"}, {"Ancillary Justice", "Ann Leckie"},
  };

  for (int i = 0; i < 6; i++) {
    const int col = i % 3;
    const int row = i / 3;
    const int tileX = pad + col * tileW;
    const int tileY = gridStartY + row * tileH;

    // Cover placeholder: outline + dark fill for lower two-thirds (mimics real cover rendering)
    renderer.drawRect(tileX + hPad, tileY + hPad, tileW - 2 * hPad, coverH, true);
    renderer.fillRect(tileX + hPad, tileY + hPad + coverH / 3, tileW - 2 * hPad, 2 * coverH / 3, true);

    const int titleY = tileY + coverH + hPad + 4;
    renderer.drawText(UI_10_FONT_ID, tileX + hPad, titleY, books[i].title, true);
    renderer.drawText(SMALL_FONT_ID, tileX + hPad, titleY + 16, books[i].author, true);
  }

  // 2×2 button menu below the cover grid.
  // menuStartY = gridStartY + 2×tileH + verticalSpacing(16) = 452
  // menuH = 800 - menuStartY - verticalSpacing(16) - buttonHintsHeight(40) = 292
  constexpr int menuStartY = gridStartY + 2 * tileH + 16;
  constexpr int menuH = 800 - menuStartY - 16 - 40;
  constexpr int menuSpacing = 8;
  constexpr int btnW = (480 - 2 * pad - menuSpacing) / 2;  // 216
  constexpr int btnH = (menuH - menuSpacing) / 2;          // 142

  static constexpr const char* menuLabels[4] = {"My Library", "Agenda", "File Transfer", "Settings"};
  for (int i = 0; i < 4; i++) {
    const int col = i % 2;
    const int row = i / 2;
    const int x = pad + col * (btnW + menuSpacing);
    const int y = menuStartY + row * (btnH + menuSpacing);
    renderer.drawRoundedRect(x, y, btnW, btnH, (i == 0) ? 2 : 1, 6, true);
    renderer.drawRect(x + 14, y + (btnH - 24) / 2, 24, 24, true);  // icon placeholder
    renderer.drawText(UI_10_FONT_ID, x + 46, y + (btnH - 14) / 2, menuLabels[i], true);
  }

  renderer.drawButtonHints(UI_10_FONT_ID, "", "Select", "Up", "Down");
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawSettingsMock(GfxRenderer& renderer) {
  renderer.clearScreen();
  drawHeader(renderer, "Settings");

  // Inline helpers to avoid repeating boilerplate for each row.
  // Groups: Reading(4), Display(3), Network(2), Sleep(2), About(2) = 13 rows.
  // Each row: 42 px tall, 4 px gap between rows, 10 px gap before group label.
  // Final row ends at Y=758, filling the screen up to the button hints.
  const auto drawGroupLabel = [&](int y, const char* label) {
    renderer.drawText(SMALL_FONT_ID, 24, y, label, true, EpdFontFamily::BOLD);
  };
  const auto drawRow = [&](int y, const char* label, const char* value) {
    renderer.drawRoundedRect(24, y, 432, 42, 1, 6, true);
    renderer.drawText(UI_10_FONT_ID, 34, y + 14, label, true);
    renderer.drawText(UI_10_FONT_ID, 300, y + 14, value, true, EpdFontFamily::BOLD);
  };

  drawGroupLabel(60, "Reading");
  drawRow(76, "Font Family", "Bookerly");
  drawRow(122, "Font Size", "14 pt");
  drawRow(168, "Line Spacing", "Normal");
  drawRow(214, "Screen Margin", "Medium");

  drawGroupLabel(266, "Display");
  drawRow(282, "UI Theme", "ForkDrift");
  drawRow(328, "E-Ink Refresh", "5 pages");
  drawRow(374, "Orientation", "Portrait");

  drawGroupLabel(426, "Network");
  drawRow(442, "WiFi Network", "HomeNetwork");
  drawRow(488, "Auto-Connect", "On");

  drawGroupLabel(540, "Sleep");
  drawRow(556, "Sleep Screen", "Custom");
  drawRow(602, "Source Folder", "/sleep");

  drawGroupLabel(654, "About");
  drawRow(670, "Device Name", "crosspoint");
  drawRow(716, "Firmware", "v0.9.2");

  renderer.drawButtonHints(UI_10_FONT_ID, "Back", "Edit", "Prev", "Next");
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawFactoryResetMock(GfxRenderer& renderer) {
  renderer.clearScreen();
  drawHeader(renderer, "Factory Reset");

  // Large warning card fills most of the screen, with Cancel/Reset buttons inside.
  // Avoids the previous layout where content only appeared in a small band at screen center.
  renderer.drawRoundedRect(20, 56, 440, 580, 2, 8, true);

  int y = 76;  // cardY + 20

  renderer.drawCenteredText(UI_12_FONT_ID, y, "Permanent Action", true, EpdFontFamily::BOLD);
  y += 34;  // 110

  renderer.drawLine(34, y, 446, y);
  y += 16;  // 126

  renderer.drawCenteredText(UI_10_FONT_ID, y, "The following data will be erased:", true);
  y += 26;  // 152

  static constexpr const char* erased[] = {
      "• Settings and preferences",
      "• WiFi and network credentials",
      "• Reading progress and bookmarks",
      "• Daily notes and todo entries",
      "• Feature store configuration",
      "• Anki cards and study data",
      "• Cover image cache",
      "• EPUB layout cache",
  };
  for (const char* item : erased) {
    renderer.drawText(UI_10_FONT_ID, 38, y, item, true);
    y += 22;
  }
  // After 8 items × 22 px: y = 152 + 176 = 328

  y += 10;  // 338
  renderer.drawLine(34, y, 446, y);
  y += 16;  // 354

  renderer.drawCenteredText(UI_10_FONT_ID, y, "Books and files on the SD card", true, EpdFontFamily::BOLD);
  y += 24;  // 378
  renderer.drawCenteredText(UI_10_FONT_ID, y, "will NOT be deleted.", true, EpdFontFamily::BOLD);
  y += 34;  // 412

  renderer.drawLine(34, y, 446, y);
  y += 16;  // 428

  renderer.drawCenteredText(UI_10_FONT_ID, y, "This action cannot be undone.", true);
  y += 34;  // 462

  // Cancel / Reset buttons
  renderer.drawRoundedRect(34, y, 190, 48, 1, 6, true);
  renderer.drawText(UI_10_FONT_ID, 90, y + 17, "Cancel", true);
  renderer.drawRoundedRect(236, y, 190, 48, 2, 6, true);
  renderer.drawText(UI_10_FONT_ID, 282, y + 17, "Reset Device", true, EpdFontFamily::BOLD);

  renderer.drawButtonHints(UI_10_FONT_ID, "Cancel", "Reset", "", "");
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawReaderMock(GfxRenderer& renderer) {
  renderer.clearScreen();
  drawHeader(renderer, "Reader");

  renderer.drawText(BOOKERLY_14_FONT_ID, 28, 62, "We walked through the quiet city as", true);
  renderer.drawText(BOOKERLY_14_FONT_ID, 28, 88, "the daylight thinned into a pale", true);
  renderer.drawText(BOOKERLY_14_FONT_ID, 28, 114, "silver dusk over the harbor.", true);
  renderer.drawText(BOOKERLY_14_FONT_ID, 28, 140, "", true);
  renderer.drawText(BOOKERLY_14_FONT_ID, 28, 166, "No one spoke. The only sound was", true);
  renderer.drawText(BOOKERLY_14_FONT_ID, 28, 192, "the wind pressing at the shutters", true);
  renderer.drawText(BOOKERLY_14_FONT_ID, 28, 218, "and the pages turning in my hand.", true);

  renderer.drawRect(28, 760, 424, 10, 1, true);
  renderer.fillRect(30, 762, 242, 6, true);
  renderer.drawText(SMALL_FONT_ID, 30, 736, "Chapter 8", true);
  renderer.drawText(SMALL_FONT_ID, 394, 736, "57%", true);

  renderer.drawButtonHints(UI_10_FONT_ID, "Menu", "Select", "Prev", "Next");
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawFeatureStoreMock(GfxRenderer& renderer) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Update", true, EpdFontFamily::BOLD);

  // Section header + navigation counter (mirrors OtaUpdateActivity SELECTING_FEATURE_STORE_BUNDLE)
  renderer.drawText(UI_10_FONT_ID, 15, 55, "Feature Store", true, EpdFontFamily::BOLD);
  const char* counter = "3 / 5";
  const int counterW = renderer.getTextWidth(UI_10_FONT_ID, counter);
  renderer.drawText(UI_10_FONT_ID, renderer.getScreenWidth() - counterW - 15, 55, counter);

  // Card border
  const int cardX = 15;
  const int cardY = 78;
  const int cardW = renderer.getScreenWidth() - 30;
  const int cardH = 510;
  renderer.drawRoundedRect(cardX, cardY, cardW, cardH, 2, 8, true);

  int y = cardY + 24;
  const int textX = cardX + 14;

  // Bundle name
  renderer.drawText(UI_12_FONT_ID, textX, y, "Latest Standard (on push)", true, EpdFontFamily::BOLD);
  y += 32;

  // Installed badge
  renderer.drawText(UI_10_FONT_ID, textX, y, "* Installed *");
  y += 22;

  // Version (no "v" prefix — "dev" is a channel label, not a numeric version)
  renderer.drawText(UI_10_FONT_ID, textX, y, "dev");
  y += 26;

  // Divider
  renderer.drawLine(cardX + 10, y, cardX + cardW - 10, y);
  y += 15;

  // Feature bullets (FEATURE_ prefix stripped, underscores → spaces)
  const char* features[] = {"• EPUB", "• FONTS", "• OTA", "• DARK MODE", "• BLE SETUP", "• WEB SETUP", "• USB STORAGE"};
  for (const auto* f : features) {
    renderer.drawText(UI_10_FONT_ID, textX + 6, y, f);
    y += 20;
  }

  renderer.drawButtonHints(UI_10_FONT_ID, "Back", "Select", "Prev", "Next");
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

}  // namespace

int main(int argc, char* argv[]) {
  const std::filesystem::path outputDir = (argc > 1) ? argv[1] : "build/screen-previews";
  std::filesystem::create_directories(outputDir);

  HalDisplay display;
  display.begin();

  GfxRenderer renderer(display);
  renderer.begin();
  FontDecompressor fontDecompressor;
  if (!fontDecompressor.init()) {
    std::cerr << "failed to initialize FontDecompressor\n";
    return 1;
  }
  renderer.setFontDecompressor(&fontDecompressor);
  installFonts(renderer);

  HalGPIO gpio;
  MappedInputManager mappedInput(gpio);

  const std::vector<std::pair<std::string, std::function<void()>>> scenarios = {
      {"01_boot", [&] { drawBoot(renderer, mappedInput); }},
      {"02_home_mock", [&] { drawHomeMock(renderer); }},
      {"03_settings_mock", [&] { drawSettingsMock(renderer); }},
      {"04_factory_reset_mock", [&] { drawFactoryResetMock(renderer); }},
      {"05_reader_mock", [&] { drawReaderMock(renderer); }},
      {"06_feature_store_mock", [&] { drawFeatureStoreMock(renderer); }},
  };

  for (const auto& [name, render] : scenarios) {
    render();
    saveSnapshot(display, outputDir, name);
  }

  return 0;
}
