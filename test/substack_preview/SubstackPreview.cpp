// Renders each state of SubstackActivity's screen (src/activities/substack/
// SubstackActivity.cpp) to a BMP file, using the real GfxRenderer/EpdFont
// code — same drawing primitives, same fonts, same pixel output the X4
// produces — without needing real hardware or the full firmware build
// (FreeRTOS tasks, Wi-Fi, SD card, FreeInkUI's battery/theme chrome).
//
// The header/button-menu/wrapped-text layout below is a hand-mirrored copy
// of BaseTheme::drawHeader / BaseTheme::drawButtonMenu / UITheme::
// drawCenteredWrappedText (src/components/themes/BaseTheme.cpp,
// src/components/UITheme.cpp) — those real methods pull in FreeInkUI +
// HalPowerManager + CrossPointSettings, which this tool has no need for.
// If SubstackActivity::render()'s real layout changes, mirror the change
// here too.
#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <builtinFonts/ubuntu_10_bold.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_12_bold.h>
#include <builtinFonts/ubuntu_12_regular.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "fontIds.h"

namespace {

constexpr int HEADER_HEIGHT = 45;
constexpr int TOP_PADDING = 5;
constexpr int BUTTON_HINTS_HEIGHT = 40;
constexpr int CONTENT_SIDE_PADDING = 20;
constexpr int MENU_ROW_HEIGHT = 45;
constexpr int MENU_SPACING = 8;
constexpr int VERTICAL_SPACING = 10;
constexpr int MAX_STATUS_LINES = 3;

// Mirrors UITheme::drawCenteredText (src/components/UITheme.cpp).
void drawCenteredText(const GfxRenderer& renderer, int x, int width, int fontId, int y, const char* text,
                      bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int tx = x + (width - renderer.getTextWidth(fontId, text, style)) / 2;
  renderer.drawText(fontId, tx, y, text, black, style);
}

// Mirrors UITheme::drawCenteredWrappedText (src/components/UITheme.cpp),
// CENTER vertical alignment only (the only mode SubstackActivity uses).
void drawCenteredWrappedText(const GfxRenderer& renderer, int x, int y, int width, int height, int fontId,
                             const char* text, int maxLines) {
  if (!text || !*text || width <= 0 || height <= 0 || maxLines <= 0) return;
  const int lineHeight = renderer.getLineHeight(fontId);
  if (lineHeight <= 0) return;
  const int lineLimit = std::min(maxLines, height / lineHeight);
  if (lineLimit <= 0) return;

  if (renderer.getTextWidth(fontId, text) <= width) {
    drawCenteredText(renderer, x, width, fontId, y + (height - lineHeight) / 2, text);
    return;
  }
  const auto lines = renderer.wrappedText(fontId, text, width, lineLimit);
  int ty = y + (height - static_cast<int>(lines.size()) * lineHeight) / 2;
  for (const auto& line : lines) {
    drawCenteredText(renderer, x, width, fontId, ty, line.c_str());
    ty += lineHeight;
  }
}

// Mirrors BaseTheme::drawButtonMenu (src/components/themes/BaseTheme.cpp).
void drawButtonMenu(GfxRenderer& renderer, int x, int y, int width, const std::vector<std::string>& labels,
                    int selectedIndex) {
  for (size_t i = 0; i < labels.size(); ++i) {
    const int tileY = VERTICAL_SPACING + y + static_cast<int>(i) * (MENU_ROW_HEIGHT + MENU_SPACING);
    const bool selected = selectedIndex == static_cast<int>(i);
    if (selected) {
      renderer.fillRect(x + CONTENT_SIDE_PADDING, tileY, width - CONTENT_SIDE_PADDING * 2, MENU_ROW_HEIGHT);
    } else {
      renderer.drawRect(x + CONTENT_SIDE_PADDING, tileY, width - CONTENT_SIDE_PADDING * 2, MENU_ROW_HEIGHT);
    }
    const char* label = labels[i].c_str();
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = x + (width - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY = tileY + (MENU_ROW_HEIGHT - lineHeight) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, !selected);
  }
}

// Simplified stand-in for BaseTheme::drawHeader: same title, same bold
// UI_12 font, same bottom rule — skips the FreeInkUI battery/status
// component, which is identical on every screen in the app and isn't
// specific to any one screen's own content. `headerBottom` is the rule
// line's y — SubstackActivity uses HEADER_HEIGHT (45), HomeActivity uses
// homeTopPadding (40) instead (see HomeActivity::render's header Rect).
// title may be null (HomeActivity passes null unless a book was recently
// read and the theme shows "Continue Reading" — Classic doesn't).
void drawHeader(GfxRenderer& renderer, int width, int headerBottom, const char* title) {
  if (title && *title) {
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    drawCenteredText(renderer, 0, width, UI_12_FONT_ID, TOP_PADDING + (headerBottom - TOP_PADDING - lineHeight) / 2,
                     title, true, EpdFontFamily::BOLD);
  }
  renderer.drawLine(0, headerBottom, width, headerBottom);
}

void drawButtonHints(GfxRenderer& renderer, int width, int height, const std::vector<std::string>& hints) {
  const int y = height - BUTTON_HINTS_HEIGHT;
  renderer.drawLine(0, y, width, y);
  const int slot = width / static_cast<int>(hints.size());
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int textY = y + (BUTTON_HINTS_HEIGHT - lineHeight) / 2;
  for (size_t i = 0; i < hints.size(); ++i) {
    if (hints[i].empty()) continue;
    drawCenteredText(renderer, static_cast<int>(i) * slot, slot, UI_10_FONT_ID, textY, hints[i].c_str());
  }
}

}  // namespace

int main() {
  // Named EpdFont objects (not new'd temporaries) so they outlive the
  // EpdFontFamily copies GfxRenderer's fontMap stores — matches main.cpp's
  // own font-registration pattern (src/main.cpp:119-125).
  static EpdFont ui10Regular(&ubuntu_10_regular);
  static EpdFont ui10Bold(&ubuntu_10_bold);
  static EpdFont ui12Regular(&ubuntu_12_regular);
  static EpdFont ui12Bold(&ubuntu_12_bold);

  display.begin();
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.setOrientation(GfxRenderer::Portrait);

  renderer.insertFont(UI_10_FONT_ID, EpdFontFamily(&ui10Regular, &ui10Bold));
  renderer.insertFont(UI_12_FONT_ID, EpdFontFamily(&ui12Regular, &ui12Bold));

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int contentTop = HEADER_HEIGHT;
  const int contentHeight = pageHeight - HEADER_HEIGHT - BUTTON_HINTS_HEIGHT;

  std::printf("Canvas: %dx%d (logical, Portrait orientation)\n", pageWidth, pageHeight);

  const auto renderState = [&](const char* filename, auto drawContent) {
    renderer.clearScreen();
    drawHeader(renderer, pageWidth, HEADER_HEIGHT, "Substacks");
    drawContent();
    HalDisplay::setNextOutputPath(std::string("test/substack_preview/out/") + filename);
    renderer.displayBuffer();
    std::printf("  wrote %s\n", filename);
  };

  // HOME — mirrors HomeActivity::render (src/activities/home/HomeActivity.cpp)
  // for a freshly-booted device (no recent books): no header title, no cover
  // tile (drawRecentBookCover has nothing to show and pulls in FreeInkUI's
  // storage/bitmap pipeline this tool skips), just the main menu. Metrics
  // below are BaseMetrics::values (components/themes/BaseTheme.h) for the
  // default Classic theme.
  {
    constexpr int HOME_TOP_PADDING = 40;    // metrics.homeTopPadding
    constexpr int HOME_COVER_HEIGHT = 400;  // metrics.homeCoverTileHeight (blank: no recent book)
    constexpr int HOME_MENU_TOP_OFFSET = 10;  // metrics.homeMenuTopOffset

    renderer.clearScreen();
    drawHeader(renderer, pageWidth, HOME_TOP_PADDING, nullptr);

    const int menuTop = HOME_TOP_PADDING + HOME_COVER_HEIGHT + HOME_MENU_TOP_OFFSET;
    const std::vector<std::string> menuLabels = {"Browse Files", "Recent Books", "File Transfer", "Substacks",
                                                 "Settings"};
    drawButtonMenu(renderer, 0, menuTop, pageWidth, menuLabels, 0);

    drawButtonHints(renderer, pageWidth, pageHeight, {"", "Select", "Up", "Down"});
    HalDisplay::setNextOutputPath("test/substack_preview/out/home.bmp");
    renderer.displayBuffer();
    std::printf("  wrote home.bmp\n");
  }

  // NOT_CONFIGURED
  renderState("not_configured.bmp", [&] {
    drawButtonHints(renderer, pageWidth, pageHeight, {"Back", "", "", ""});
    drawCenteredWrappedText(renderer, 0, contentTop, pageWidth, contentHeight, UI_12_FONT_ID,
                            "Set the CrossInk API URL and Device ID in Settings.", MAX_STATUS_LINES);
  });

  // FETCHING
  renderState("fetching.bmp", [&] {
    drawButtonHints(renderer, pageWidth, pageHeight, {"Back", "", "", ""});
    drawCenteredWrappedText(renderer, 0, contentTop, pageWidth, contentHeight, UI_12_FONT_ID, "Loading",
                            MAX_STATUS_LINES);
  });

  // EMPTY
  renderState("empty.bmp", [&] {
    drawButtonHints(renderer, pageWidth, pageHeight, {"Back", "", "", ""});
    drawCenteredWrappedText(renderer, 0, contentTop, pageWidth, contentHeight, UI_12_FONT_ID,
                            "No new EPUBs. Refresh from the CrossInk dashboard.", MAX_STATUS_LINES);
  });

  // ERROR
  renderState("error.bmp", [&] {
    drawButtonHints(renderer, pageWidth, pageHeight, {"Back", "", "", ""});
    drawCenteredWrappedText(renderer, 0, contentTop, pageWidth, contentHeight, UI_12_FONT_ID,
                            "Could not reach the CrossInk API. Check your Wi-Fi and API URL.", MAX_STATUS_LINES);
  });

  // DOWNLOADING
  renderState("downloading.bmp", [&] {
    drawButtonHints(renderer, pageWidth, pageHeight, {"Back", "", "", ""});
    drawCenteredWrappedText(renderer, 0, contentTop, pageWidth, contentHeight, UI_12_FONT_ID, "Downloading… 42%",
                            MAX_STATUS_LINES);
  });

  // LIST — sample pending EPUBs, matching what GET /device/:id/pending
  // returns (title + profileName), selection on the first row.
  renderState("list.bmp", [&] {
    drawButtonHints(renderer, pageWidth, pageHeight, {"Back", "Select", "Up", "Down"});
    const std::vector<std::string> labels = {
        "TIRINHAS FRITAS OLEOSAS \xe2\x80\x94 Matinal Tecnologia",
        "3 articles: The silent collapse of attention \xe2\x80\x94 The Convivial Society",
        "We're Hiring \xe2\x80\x94 Dense Discovery",
    };
    drawButtonMenu(renderer, 0, contentTop, pageWidth, labels, 0);
  });

  std::printf("Done. BMPs written under test/substack_preview/out/\n");
  return 0;
}
