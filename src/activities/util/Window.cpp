#include "./Window.h"
#include "Battery.h"
#include "config.h"

namespace {
constexpr int windowCornerRadius = 16;
constexpr int windowBorderWidth = 2;
constexpr int fullscreenWindowMargin = 20;
constexpr int windowHeaderHeight = 50;
constexpr int statusBarHeight = 50;
constexpr int batteryWidth = 15;
constexpr int batteryHeight = 10;
}  // namespace

void drawWindowFrame(GfxRenderer& renderer, int xMargin, int y, int height, bool hasShadow, const char* title) {
    const int windowWidth = GfxRenderer::getScreenWidth() - 2 * xMargin;
    renderer.drawRoundedRect(xMargin, y, windowWidth, height, windowBorderWidth, windowCornerRadius, true);

    if (hasShadow) {
        renderer.drawLine(windowWidth + xMargin, y + windowCornerRadius + 2, windowWidth + xMargin, y + height - windowCornerRadius, windowBorderWidth, true);
        renderer.drawLine(xMargin + windowCornerRadius + 2, y + height, windowWidth + xMargin - windowCornerRadius, y + height, windowBorderWidth, true);
        renderer.drawArc(windowCornerRadius + windowBorderWidth, windowWidth + xMargin - 1 - windowCornerRadius, y + height - 1 - windowCornerRadius, 1, 1, windowBorderWidth, true);
        renderer.drawPixel(xMargin + windowCornerRadius + 1, y + height, true);
    }

    if (title) { // Header
        const int titleWidth = renderer.getTextWidth(UI_FONT_ID, title);
        const int titleX = (GfxRenderer::getScreenWidth() - titleWidth) / 2;
        const int titleY = y + 10;
        renderer.drawText(UI_FONT_ID, titleX, titleY, title, true, REGULAR);
        renderer.drawLine(xMargin, y + windowHeaderHeight, windowWidth + xMargin, y + windowHeaderHeight, windowBorderWidth, true);
    }
}

void drawFullscreenWindowFrame(GfxRenderer& renderer, const char* title) {
    drawStatusBar(renderer);
    drawWindowFrame(renderer, fullscreenWindowMargin, statusBarHeight, GfxRenderer::getScreenHeight() - fullscreenWindowMargin - statusBarHeight, true, title);
}

void drawStatusBar(GfxRenderer& renderer) {
    constexpr auto textY = 18;

    // Left aligned battery icon and percentage
    const uint16_t percentage = battery.readPercentage();
    const auto percentageText = std::to_string(percentage) + "%";
    const auto percentageTextWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
    renderer.drawText(SMALL_FONT_ID, fullscreenWindowMargin + batteryWidth + 5, textY, percentageText.c_str());

    // 1 column on left, 2 columns on right, 5 columns of battery body
    constexpr int x = fullscreenWindowMargin;
    constexpr int y = textY + 5;

    // Top line
    renderer.drawLine(x, y, x + batteryWidth - 4, y);
    // Bottom line
    renderer.drawLine(x, y + batteryHeight - 1, x + batteryWidth - 4, y + batteryHeight - 1);
    // Left line
    renderer.drawLine(x, y, x, y + batteryHeight - 1);
    // Battery end
    renderer.drawLine(x + batteryWidth - 4, y, x + batteryWidth - 4, y + batteryHeight - 1);
    renderer.drawLine(x + batteryWidth - 3, y + 2, x + batteryWidth - 1, y + 2);
    renderer.drawLine(x + batteryWidth - 3, y + batteryHeight - 3, x + batteryWidth - 1, y + batteryHeight - 3);
    renderer.drawLine(x + batteryWidth - 1, y + 2, x + batteryWidth - 1, y + batteryHeight - 3);

    // The +1 is to round up, so that we always fill at least one pixel
    int filledWidth = percentage * (batteryWidth - 5) / 100 + 1;
    if (filledWidth > batteryWidth - 5) {
        filledWidth = batteryWidth - 5;  // Ensure we don't overflow
    }
    renderer.fillRect(x + 1, y + 1, filledWidth, batteryHeight - 2);
}