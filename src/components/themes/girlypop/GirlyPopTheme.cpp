#include "GirlyPopTheme.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <string>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr int kTitleFontId = UI_12_FONT_ID;
constexpr int kGuideFontId = SMALL_FONT_ID;
constexpr int kTabFontId = UI_12_FONT_ID;

// Double-frame border inset distances and corner radii.
constexpr int kOuterInset = 4;
constexpr int kInnerInset = 9;
constexpr int kOuterRadius = 14;
constexpr int kInnerRadius = 9;
// How far from the edge the diamond decorators sit (between the two frames).
constexpr int kStarGap = 2;

// Draw a small diamond shape (5 filled 2x2 pixels) centred on (cx, cy).
void drawDiamond(const GfxRenderer& renderer, int cx, int cy) {
  constexpr int arm = 4;
  constexpr int dotSize = 2;
  renderer.fillRect(cx, cy - arm, dotSize, dotSize, true);      // top
  renderer.fillRect(cx - arm, cy, dotSize, dotSize, true);      // left
  renderer.fillRect(cx, cy, dotSize, dotSize, true);            // centre
  renderer.fillRect(cx + arm, cy, dotSize, dotSize, true);      // right
  renderer.fillRect(cx, cy + arm, dotSize, dotSize, true);      // bottom
}

}  // namespace

void GirlyPopTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                               const char* subtitle) const {
  (void)subtitle;
  if (title == nullptr) {
    return;
  }

  // --- Double-frame border ---
  renderer.drawRoundedRect(rect.x + kOuterInset, rect.y + kOuterInset, rect.width - 2 * kOuterInset,
                           rect.height - 2 * kOuterInset, 1, kOuterRadius, true);
  renderer.drawRoundedRect(rect.x + kInnerInset, rect.y + kInnerInset, rect.width - 2 * kInnerInset,
                           rect.height - 2 * kInnerInset, 1, kInnerRadius, true);

  const int centerY = rect.y + rect.height / 2;

  // --- Diamond decorators on the left side between frames ---
  const int starX = rect.x + kOuterInset + kStarGap + 2;
  drawDiamond(renderer, starX, centerY - 1);

  // --- Battery right-aligned, inside the inner frame ---
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int sidePadding = GirlyPopMetrics::values.contentSidePadding;
  const int batteryIconX = rect.x + rect.width - sidePadding - GirlyPopMetrics::values.batteryWidth;

  int batteryGroupLeftX = batteryIconX;
  if (showBatteryPercentage) {
    const int maxTextWidth = renderer.getTextWidth(kGuideFontId, "100%");
    batteryGroupLeftX -= maxTextWidth + batteryPercentSpacing;

    const int clearW = maxTextWidth + batteryPercentSpacing + GirlyPopMetrics::values.batteryWidth;
    const int clearH = std::max(renderer.getTextHeight(kGuideFontId), GirlyPopMetrics::values.batteryHeight + 8);
    renderer.fillRect(batteryIconX - maxTextWidth - batteryPercentSpacing, rect.y + kInnerInset + 4, clearW, clearH,
                      false);
  }

  drawBatteryRight(renderer,
                   Rect{batteryIconX, rect.y + kInnerInset + 4, GirlyPopMetrics::values.batteryWidth,
                        GirlyPopMetrics::values.batteryHeight},
                   showBatteryPercentage);

  // --- Title: centred horizontally, italic serif, truncated to avoid the battery ---
  const int titleY = centerY - renderer.getLineHeight(kTitleFontId) / 2;
  const int rightReserved = rect.width - batteryGroupLeftX + sidePadding;
  const int maxTitleWidth = std::max(0, rect.width - kInnerInset * 2 - 30 - rightReserved);
  auto headerTitle = renderer.truncatedText(kTitleFontId, title, maxTitleWidth, EpdFontFamily::ITALIC);
  renderer.drawCenteredText(kTitleFontId, titleY, headerTitle.c_str(), true, EpdFontFamily::ITALIC);
}

void GirlyPopTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                               bool selected) const {
  if (tabs.empty()) {
    return;
  }

  const int slotWidth = rect.width / static_cast<int>(tabs.size());
  constexpr int tabInset = 6;
  const int tabY = rect.y + tabInset;
  const int tabHeight = rect.height - tabInset * 2;
  constexpr int tabRadius = 20;

  for (size_t i = 0; i < tabs.size(); i++) {
    const int slotX = rect.x + static_cast<int>(i) * slotWidth;
    const int tabX = slotX + tabInset;
    const int tabWidth = slotWidth - tabInset * 2;
    const auto& tab = tabs[i];

    if (tab.selected) {
      renderer.fillRoundedRect(tabX, tabY, tabWidth, tabHeight, tabRadius, selected ? Color::Black : Color::DarkGray);
    } else {
      // Unselected: subtle outline pill
      renderer.drawRoundedRect(tabX, tabY, tabWidth, tabHeight, 1, tabRadius, true);
    }

    const int textWidth = renderer.getTextWidth(kTabFontId, tab.label, EpdFontFamily::BOLD);
    const int textX = slotX + (slotWidth - textWidth) / 2;
    const int textY = tabY + (tabHeight - renderer.getLineHeight(kTabFontId)) / 2;
    renderer.drawText(kTabFontId, textX, textY, tab.label, !tab.selected, EpdFontFamily::BOLD);
  }

  // Decorative dotted line below the tab bar
  const int dotY = rect.y + rect.height - 2;
  constexpr int dotSpacing = 8;
  constexpr int dotSize = 2;
  for (int x = rect.x; x < rect.x + rect.width - dotSize; x += dotSpacing) {
    renderer.fillRect(x, dotY, dotSize, dotSize, true);
  }
}
