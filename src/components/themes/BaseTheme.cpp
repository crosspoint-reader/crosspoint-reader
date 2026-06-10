#include "BaseTheme.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include <FreeInkUIGfxRenderer.h>

#include "I18n.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include <HalGPIO.h>
#include <I18n.h>
#include <cmath>
#include <vector>
#include "components/icons/book.h"
#include "components/icons/book24.h"
#include "components/icons/bookmark.h"
#include "components/icons/cover.h"
#include "components/icons/file24.h"
#include "components/icons/folder.h"
#include "components/icons/folder24.h"
#include "components/icons/hotspot.h"
#include "components/icons/image24.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/text24.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"

// Internal constants
namespace {
constexpr int homeMenuMargin = 20;
constexpr int homeMarginTop = 30;
constexpr int subtitleY = 738;

namespace fui = freeink::ui;

// Render-only FreeInkUI frame over the renderer with the standard font slots
// bound. Theme drawing is draw-only, so no interactions are registered.
struct UiFrame {
  fui::GfxRendererTarget target;
  fui::InteractionBuffer<1> interactions;
  fui::InputSnapshot input{};
  fui::DeviceContext device;
  fui::Frame<1> frame;

  explicit UiFrame(const GfxRenderer& renderer)
      : target(renderer), device(target.deviceContext()), frame(target, device, input, interactions) {
    target.setFont(fui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
    target.setFont(fui::GfxRendererTarget::FONT_BODY, UI_12_FONT_ID);
    target.setFont(fui::GfxRendererTarget::FONT_TITLE, UI_12_FONT_ID);
  }
};

fui::TextStyle uiText(const fui::FontId font, const fui::TextAlign align = fui::TextAlign::Left,
                      const bool bold = false, const fui::Color color = fui::Color::Black) {
  fui::TextStyle style;
  style.font = font;
  style.align = align;
  style.bold = bold;
  style.color = color;
  style.inverted = color == fui::Color::White;
  return style;
}

}  // namespace

void BaseTheme::drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight) {
  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + rectHeight - 1, x + battWidth - 3, y + rectHeight - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rectHeight - 2);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rectHeight - 2);
  renderer.drawPixel(x + battWidth - 1, y + 3);
  renderer.drawPixel(x + battWidth - 1, y + rectHeight - 4);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rectHeight - 5);
}

void BaseTheme::drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY) {
  // Draw lightning bolt (white/inverted on black fill for visibility)
  renderer.drawLine(boltX + 4, boltY + 0, boltX + 5, boltY + 0, false);
  renderer.drawLine(boltX + 3, boltY + 1, boltX + 4, boltY + 1, false);
  renderer.drawLine(boltX + 2, boltY + 2, boltX + 5, boltY + 2, false);
  renderer.drawLine(boltX + 3, boltY + 3, boltX + 4, boltY + 3, false);
  renderer.drawLine(boltX + 2, boltY + 4, boltX + 3, boltY + 4, false);
  renderer.drawLine(boltX + 1, boltY + 5, boltX + 4, boltY + 5, false);
  renderer.drawLine(boltX + 2, boltY + 6, boltX + 3, boltY + 6, false);
  renderer.drawLine(boltX + 1, boltY + 7, boltX + 2, boltY + 7, false);
}

void BaseTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Left aligned: icon on left, percentage on right (reader mode)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(SMALL_FONT_ID, rect.x + batteryPercentSpacing + rect.width, rect.y, percentageText.c_str());
  }

  const Rect iconRect{rect.x, y, rect.width, rect.height};
  drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height);
  fillBatteryIcon(renderer, iconRect, percentage);
}

void BaseTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Right aligned: percentage on left, icon on right (UI headers)
  // rect.x is already positioned for the icon (drawHeader calculated it)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x - textWidth - batteryPercentSpacing, rect.y, percentageText.c_str());
  }

  const Rect iconRect{rect.x, y, rect.width, rect.height};
  drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height);
  fillBatteryIcon(renderer, iconRect, percentage);
}

void BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current,
                                const size_t total) const {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  LOG_DBG("UI", "Drawing progress bar: current=%u, total=%u, percent=%d", current, total, percent);
  // Draw outline
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  // Draw filled portion
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  }

  // Draw percentage text centered below bar
  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText.c_str());
}

// Draw the "Recent Book" cover card on the home screen
// TODO: Refactor method to make it cleaner, split into smaller methods
Rect BaseTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int marginX = metrics.popupMarginX;
  const int marginY = metrics.popupMarginY;
  const int frameThickness = metrics.popupFrameThickness;
  const bool rounded = metrics.popupCornerRadius > 0;

  UiFrame ui(renderer);
  const fui::TextStyle messageText = uiText(fui::GfxRendererTarget::FONT_BODY, fui::TextAlign::Center,
                                            metrics.popupTextBold,
                                            metrics.popupTextInverted ? fui::Color::Black : fui::Color::White);

  // Popup sizes to the message; y scales proportionally to screen height.
  const int y = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const fui::Size textSize = ui.target.measureText(messageText.font, message, messageText);
  const int w = textSize.width + marginX * 2;
  const int h = textSize.height + marginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  // Rounded popups are dark-on-light-frame; square popups are the reverse.
  fui::PopupProps props;
  props.message = message;
  props.text = messageText;
  // The baseline offset shifts the message without changing the popup box:
  // asymmetric padding keeps the content height identical.
  props.padding = fui::Insets{static_cast<int16_t>(marginY + frameThickness + metrics.popupTextBaselineOffsetY),
                              static_cast<int16_t>(marginX + frameThickness),
                              static_cast<int16_t>(marginY + frameThickness - metrics.popupTextBaselineOffsetY),
                              static_cast<int16_t>(marginX + frameThickness)};
  props.styles.normal.background = fui::Paint::solid(rounded ? fui::Color::Black : fui::Color::White);
  props.styles.normal.border = fui::Paint::solid(rounded ? fui::Color::White : fui::Color::Black);
  props.styles.normal.borderWidth = static_cast<uint8_t>(frameThickness);
  props.styles.normal.radius = static_cast<uint8_t>(rounded ? metrics.popupCornerRadius + frameThickness : 0);

  const fui::Rect outer{static_cast<int16_t>(x - frameThickness), static_cast<int16_t>(y - frameThickness),
                        static_cast<int16_t>(w + frameThickness * 2), static_cast<int16_t>(h + frameThickness * 2)};
  fui::popup(ui.frame, outer, props);
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

void BaseTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int barHeight = metrics.popupProgressBarHeight;
  const int barWidth =
      std::max(0, layout.width - metrics.popupMarginX * 2);  // twice the margin in drawPopup to match text width
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - metrics.popupMarginY / 2 - barHeight / 2 - 1;
  if (barWidth <= 0 || barHeight <= 0) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  UiFrame ui(renderer);
  fui::ProgressBarProps bar;
  bar.value = metrics.popupProgressClampPercent ? std::clamp(progress, 0, 100) : progress;
  bar.max = 100;
  bar.fill = fui::Paint::solid(metrics.popupProgressFillInverted ? fui::Color::Black : fui::Color::White);
  if (metrics.popupProgressDrawOutline) {
    bar.border = fui::Paint::solid(metrics.popupProgressOutlineInverted ? fui::Color::Black : fui::Color::White);
    bar.borderWidth = 1;
  }
  fui::progressBar(ui.frame,
                   fui::Rect{static_cast<int16_t>(barX), static_cast<int16_t>(barY), static_cast<int16_t>(barWidth),
                             static_cast<int16_t>(barHeight)},
                   bar);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BaseTheme::drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                              const int pageCount, std::string title, const int paddingBottom, const int textYOffset,
                              const bool fillMargin) const {
  auto metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  UiFrame ui(renderer);
  const auto screenHeight = renderer.getScreenHeight();
  const int textY = screenHeight - UITheme::getInstance().getStatusBarHeight() - orientedMarginBottom - paddingBottom - 4;

  // The bar's text row: settings decide which strings exist, the app formats
  // them, and the FreeInkUI statusBar lays out clusters and the title.
  char progressStr[32] = {0};
  if (SETTINGS.statusBarBookProgressPercentage && SETTINGS.statusBarChapterPageCount) {
    snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", currentPage, pageCount, bookProgress);
  } else if (SETTINGS.statusBarBookProgressPercentage) {
    snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
  } else if (SETTINGS.statusBarChapterPageCount) {
    snprintf(progressStr, sizeof(progressStr), "%d/%d", currentPage, pageCount);
  }

  char timeBuf[9] = {0};  // X3 only — DS3231 RTC
  if (SETTINGS.statusBarClock && halClock.isAvailable()) {
    halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1);
  }

  // Battery stays theme-drawn (Lyra overrides the glyph); the component just
  // keeps the title clear of it via leadingReserve.
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;
  int16_t batteryReserve = 0;
  if (SETTINGS.statusBarBattery) {
    GUI.drawBatteryLeft(renderer,
                        Rect{metrics.statusBarHorizontalMargin + orientedMarginLeft + 1, textY, metrics.batteryWidth,
                             metrics.batteryHeight},
                        showBatteryPercentage);
    batteryReserve = static_cast<int16_t>((showBatteryPercentage ? 50 : 20) + 30);
  }

  fui::StatusBarProps bar;
  bar.title = title.empty() ? nullptr : title.c_str();
  bar.trailing = progressStr[0] != '\0' ? progressStr : nullptr;
  bar.trailingSecondary = timeBuf[0] != '\0' ? timeBuf : nullptr;
  bar.leadingReserve = batteryReserve;
  bar.titleOffsetY = static_cast<int16_t>(textYOffset);
  bar.text = uiText(fui::GfxRendererTarget::FONT_SMALL);
  bar.horizontalPadding = static_cast<int16_t>(metrics.statusBarHorizontalMargin);
  bar.gap = 10;
  fui::statusBar(ui.frame,
                 fui::Rect{static_cast<int16_t>(orientedMarginLeft), static_cast<int16_t>(textY),
                           static_cast<int16_t>(renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight),
                           static_cast<int16_t>(renderer.getLineHeight(SMALL_FONT_ID))},
                 bar);

  // Progress bar: geometry depends on the panel margins (fillMargin extends
  // into the bottom bezel), so it draws as its own component below the text.
  if (SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS) {
    const int barMarginLeft = fillMargin ? 0 : orientedMarginLeft;
    const int barMarginRight = fillMargin ? 0 : orientedMarginRight;
    const int thickness = (SETTINGS.statusBarProgressBarThickness + 1) * 2;
    const int progressBarY = screenHeight - orientedMarginBottom - thickness - paddingBottom;
    const int progress =
        SETTINGS.statusBarProgressBar == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS
            ? static_cast<int>(bookProgress)
            : (pageCount > 0 ? static_cast<int>((static_cast<float>(currentPage) / pageCount) * 100) : 0);
    fui::ProgressBarProps progressProps;
    progressProps.value = progress;
    progressProps.max = 100;
    fui::progressBar(ui.frame,
                     fui::Rect{static_cast<int16_t>(barMarginLeft), static_cast<int16_t>(progressBarY),
                               static_cast<int16_t>(renderer.getScreenWidth() - barMarginLeft - barMarginRight),
                               static_cast<int16_t>(thickness + (fillMargin ? orientedMarginBottom - 1 : 0))},
                     progressProps);
  }
}

void BaseTheme::drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  auto truncatedLabel =
      renderer.truncatedText(SMALL_FONT_ID, label, rect.width - metrics.contentSidePadding * 2, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, rect.y, truncatedLabel.c_str());
}

void BaseTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                              int contentStartX, int contentWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineY = rect.y + rect.height + lineHeight + metrics.verticalSpacing;
  const int thickness = cursorMode ? metrics.textFieldCursorThickness : metrics.textFieldNormalThickness;
  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY,
                      rect.x + contentStartX + contentWidth + metrics.textFieldLineEndOffset, lineY, thickness, true);
  } else {
    const int lineW = textWidth + metrics.textFieldHorizontalPadding * 2;
    const int lineStart = rect.x + (rect.width - lineW) / 2;
    renderer.drawLine(lineStart, lineY, lineStart + lineW + metrics.textFieldLineEndOffset, lineY, thickness, true);
  }
}

void BaseTheme::drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, const bool isSelected,
                                const char* secondaryLabel, const KeyboardKeyType keyType,
                                const bool inactiveSelection) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const uint8_t cr = static_cast<uint8_t>(metrics.keyboardKeyCornerRadius);
  const bool isSpecialKey = keyType == KeyboardKeyType::Shift || keyType == KeyboardKeyType::Mode ||
                            keyType == KeyboardKeyType::Del || keyType == KeyboardKeyType::Space ||
                            keyType == KeyboardKeyType::Ok || keyType == KeyboardKeyType::Disabled;
  const bool shouldDrawOutline =
      (metrics.keyboardDrawSpecialOutlineWhenUnselected && isSpecialKey) || metrics.keyboardOutlineAllUnselected;

  // Key states map onto a FreeInkUI StyleSet: selected = black fill,
  // inactive selection = focused (gray fill, or thick outline when square),
  // disabled = dithered gray.
  fui::StyleSet styles;
  styles.normal.background =
      metrics.keyboardFillUnselected ? fui::Paint::solid(fui::Color::White) : fui::Paint::none();
  styles.normal.border = shouldDrawOutline ? fui::Paint::solid(fui::Color::Black) : fui::Paint::none();
  styles.normal.borderWidth = shouldDrawOutline ? 1 : 0;
  styles.normal.radius = cr;

  styles.selected.background = fui::Paint::solid(fui::Color::Black);
  styles.selected.foreground = fui::Paint::solid(fui::Color::White);
  styles.selected.radius = cr;

  styles.focused = styles.normal;
  if (cr > 0) {
    styles.focused.background = fui::Paint::dither(fui::Color::LightGray);
  } else {
    styles.focused.border = fui::Paint::solid(fui::Color::Black);
    styles.focused.borderWidth = 2;
  }

  styles.disabled.background = fui::Paint::dither(fui::Color::LightGray);
  styles.disabled.radius = cr;
  styles.active = styles.selected;

  fui::State state = fui::StateNormal;
  if (isSelected) state = inactiveSelection ? fui::StateFocused : fui::StateSelected;
  if (keyType == KeyboardKeyType::Disabled && !(isSelected && inactiveSelection)) state |= fui::StateDisabled;

  UiFrame ui(renderer);
  const fui::Rect keyRect{static_cast<int16_t>(rect.x), static_cast<int16_t>(rect.y),
                          static_cast<int16_t>(rect.width), static_cast<int16_t>(rect.height)};
  fui::ButtonProps key;
  // Space/Del draw glyph art below instead of a label.
  key.label = (keyType == KeyboardKeyType::Space || keyType == KeyboardKeyType::Del) ? nullptr : label;
  key.text = uiText(fui::GfxRendererTarget::FONT_BODY, fui::TextAlign::Center);
  key.styles = styles;
  key.state = state;
  key.minTouchSize = 0;  // keyboard owns its hit geometry
  fui::button(ui.frame, keyRect, key);

  const bool invert = isSelected && !inactiveSelection;

  if (keyType == KeyboardKeyType::Space) {
    const int lineHalfWidth = rect.width * 3 / 10;
    const int centerX = rect.x + rect.width / 2;
    const int lineY = rect.y + rect.height / 2 + 3;
    renderer.drawLine(centerX - lineHalfWidth, lineY, centerX + lineHalfWidth, lineY, 3, !invert);
    return;
  }

  if (keyType == KeyboardKeyType::Del) {
    const int centerX = rect.x + rect.width / 2;
    const int centerY = rect.y + rect.height / 2;
    const int arrowLen = rect.width / 4;
    const int arrowHead = std::max(metrics.keyboardMinArrowHeadSize, arrowLen / 2);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX + arrowLen / 2, centerY, 3, !invert);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX - arrowLen / 2 + arrowHead, centerY - arrowHead, 3,
                      !invert);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX - arrowLen / 2 + arrowHead, centerY + arrowHead, 3,
                      !invert);
    return;
  }

  if (secondaryLabel != nullptr && secondaryLabel[0] != '\0') {
    const int secWidth = renderer.getTextWidth(SMALL_FONT_ID, secondaryLabel);
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - secWidth - metrics.keyboardSecondaryLabelRightPadding,
                      rect.y + metrics.keyboardSecondaryLabelTopPadding, secondaryLabel, !invert);
  }
}

// ---- Spec-driven component implementations (formerly LyraTheme) ----

namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int topHintButtonY = 345;
constexpr int maxListValueWidth = 200;
constexpr int mainMenuIconSize = 32;
constexpr int listIconSize = 24;
constexpr int mainMenuColumns = 2;
int coverWidth = 0;

bool isBmpIconSize(int size) { return size > 0 && size <= 64; }

const uint8_t* iconForName(UIIcon icon, int size) {
  if (size == 24) {
    switch (icon) {
      case UIIcon::Folder:
        return Folder24Icon;
      case UIIcon::Text:
        return Text24Icon;
      case UIIcon::Image:
        return Image24Icon;
      case UIIcon::Book:
        return Book24Icon;
      case UIIcon::File:
        return File24Icon;
      default:
        return nullptr;
    }
  } else if (size == 32) {
    switch (icon) {
      case UIIcon::Folder:
        return FolderIcon;
      case UIIcon::Book:
        return BookIcon;
      case UIIcon::Recent:
        return RecentIcon;
      case UIIcon::Settings:
        return Settings2Icon;
      case UIIcon::Transfer:
        return TransferIcon;
      case UIIcon::Library:
        return LibraryIcon;
      case UIIcon::Wifi:
        return WifiIcon;
      case UIIcon::Hotspot:
        return HotspotIcon;
      case UIIcon::Bookmark:
        return BookmarkIcon;
      default:
        return nullptr;
    }
  }
  return nullptr;
}

enum class ButtonHintShape { None, Back, Select, Up, Down, Left, Right };

bool matchesLabel(const char* label, const char* expected) {
  return label != nullptr && expected != nullptr && strcmp(label, expected) == 0;
}

ButtonHintShape shapeForButtonHintLabel(const char* label) {
  if (label == nullptr || label[0] == '\0') return ButtonHintShape::None;
  if (matchesLabel(label, tr(STR_BACK)) || matchesLabel(label, tr(STR_CANCEL)) || matchesLabel(label, tr(STR_HOME))) {
    return ButtonHintShape::Back;
  }
  if (matchesLabel(label, tr(STR_SELECT)) || matchesLabel(label, tr(STR_CONFIRM)) ||
      matchesLabel(label, tr(STR_OK_BUTTON)) || matchesLabel(label, tr(STR_DONE)) ||
      matchesLabel(label, tr(STR_OPEN)) || matchesLabel(label, tr(STR_TOGGLE))) {
    return ButtonHintShape::Select;
  }
  if (matchesLabel(label, tr(STR_DIR_UP))) return ButtonHintShape::Up;
  if (matchesLabel(label, tr(STR_DIR_DOWN))) return ButtonHintShape::Down;
  if (matchesLabel(label, tr(STR_DIR_LEFT)) || strcmp(label, "<") == 0 || strcmp(label, "-") == 0) {
    return ButtonHintShape::Left;
  }
  if (matchesLabel(label, tr(STR_DIR_RIGHT)) || strcmp(label, ">") == 0 || strcmp(label, "+") == 0) {
    return ButtonHintShape::Right;
  }
  return ButtonHintShape::None;
}

void fillCircle(const GfxRenderer& renderer, int cx, int cy, int radius) {
  const int r2 = radius * radius;
  for (int y = -radius; y <= radius; ++y) {
    for (int x = -radius; x <= radius; ++x) {
      if (x * x + y * y <= r2) renderer.drawPixel(cx + x, cy + y, true);
    }
  }
}

void drawButtonHintShape(const GfxRenderer& renderer, ButtonHintShape shape, int centerX, int centerY, int size) {
  const int half = std::max(4, size / 2);
  if (shape == ButtonHintShape::Back) {
    renderer.fillRect(centerX - half, centerY - half, half * 2, half * 2, true);
  } else if (shape == ButtonHintShape::Select) {
    fillCircle(renderer, centerX, centerY, half);
  } else if (shape != ButtonHintShape::None) {
    int xPoints[3] = {};
    int yPoints[3] = {};
    switch (shape) {
      case ButtonHintShape::Up:
        xPoints[0] = centerX;
        yPoints[0] = centerY - half;
        xPoints[1] = centerX - half;
        yPoints[1] = centerY + half;
        xPoints[2] = centerX + half;
        yPoints[2] = centerY + half;
        break;
      case ButtonHintShape::Down:
        xPoints[0] = centerX - half;
        yPoints[0] = centerY - half;
        xPoints[1] = centerX + half;
        yPoints[1] = centerY - half;
        xPoints[2] = centerX;
        yPoints[2] = centerY + half;
        break;
      case ButtonHintShape::Left:
        xPoints[0] = centerX - half;
        yPoints[0] = centerY;
        xPoints[1] = centerX + half;
        yPoints[1] = centerY - half;
        xPoints[2] = centerX + half;
        yPoints[2] = centerY + half;
        break;
      case ButtonHintShape::Right:
        xPoints[0] = centerX - half;
        yPoints[0] = centerY - half;
        xPoints[1] = centerX + half;
        yPoints[1] = centerY;
        xPoints[2] = centerX - half;
        yPoints[2] = centerY + half;
        break;
      default:
        return;
    }
    renderer.fillPolygon(xPoints, yPoints, 3, true);
  }
}
}  // namespace

bool BaseTheme::hasThemeIcon(UIIcon icon) const {
  return assetRoot_ != nullptr && icons_ != nullptr && icons_->find(icon) != icons_->end();
}

bool BaseTheme::drawThemeIcon(const GfxRenderer& renderer, UIIcon icon, int x, int y, int size) const {
  if (assetRoot_ == nullptr || icons_ == nullptr || !isBmpIconSize(size)) return false;
  const auto it = icons_->find(icon);
  if (it == icons_->end() || it->second.empty()) return false;

  std::string path = assetRoot_;
  if (!path.empty() && path.back() != '/') path += "/";
  path += it->second;

  HalFile file;
  if (!Storage.openFileForRead("THEME", path.c_str(), file)) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;

  float scale = 1.0f;
  if (bitmap.getWidth() > size) {
    scale = std::min(scale, static_cast<float>(size) / static_cast<float>(bitmap.getWidth()));
  }
  if (bitmap.getHeight() > size) {
    scale = std::min(scale, static_cast<float>(size) / static_cast<float>(bitmap.getHeight()));
  }
  const int drawnWidth = std::max(1, static_cast<int>(std::floor(bitmap.getWidth() * scale)));
  const int drawnHeight = std::max(1, static_cast<int>(std::floor(bitmap.getHeight() * scale)));
  renderer.drawBitmap(bitmap, x + (size - drawnWidth) / 2, y + (size - drawnHeight) / 2, drawnWidth, drawnHeight);
  return true;
}

void BaseTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const bool charging = gpio.isUsbConnected();

  if (charging) {
    // Solid fill when charging so lightning bolt is visible
    renderer.fillRect(rect.x + 2, rect.y + 2, rect.width - 5, rect.height - 4);
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
  } else {
    if (percentage > 10) {
      renderer.fillRect(rect.x + 2, rect.y + 2, 3, rect.height - 4);
    }
    if (percentage > 40) {
      renderer.fillRect(rect.x + 6, rect.y + 2, 3, rect.height - 4);
    }
    if (percentage > 70) {
      renderer.fillRect(rect.x + 10, rect.y + 2, 3, rect.height - 4);
    }
  }
}

void BaseTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  if (header_ != nullptr && header_->enabled) {
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

    const bool showBatteryPercentage =
        SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
    const int batteryX = rect.x + rect.width - metrics().contentSidePadding - metrics().batteryWidth;
    drawBatteryRight(renderer,
                     Rect{batteryX, rect.y + header_->batteryOffsetY, metrics().batteryWidth, metrics().batteryHeight},
                     showBatteryPercentage);

    const auto style = header_->bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    if (title != nullptr) {
      const int reserveRight = rect.width - batteryX + metrics().batteryWidth + metrics().contentSidePadding;
      const int maxTitleWidth = std::max(0, rect.width - reserveRight * 2);
      const auto truncatedTitle = renderer.truncatedText(header_->fontId, title, maxTitleWidth, style);
      const int lineHeight = renderer.getLineHeight(header_->fontId);
      const int titleY = rect.y + header_->titleOffsetY + std::max(0, (rect.height - lineHeight) / 2);
      if (header_->centeredTitle) {
        const int textWidth = renderer.getTextWidth(header_->fontId, truncatedTitle.c_str(), style);
        renderer.drawText(header_->fontId, rect.x + (rect.width - textWidth) / 2, titleY, truncatedTitle.c_str(), true,
                          style);
      } else {
        renderer.drawText(header_->fontId, rect.x + metrics().contentSidePadding, titleY, truncatedTitle.c_str(), true,
                          style);
      }
    }

    if (subtitle != nullptr && subtitle[0] != '\0') {
      const auto truncatedSubtitle =
          renderer.truncatedText(SMALL_FONT_ID, subtitle, rect.width - metrics().contentSidePadding * 2);
      const int subtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
      renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - metrics().contentSidePadding - subtitleWidth,
                        rect.y + rect.height - renderer.getLineHeight(SMALL_FONT_ID) - 4, truncatedSubtitle.c_str(),
                        true);
    }

    if (header_->showDivider) {
      renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
    }
    return;
  }

  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // Position icon at right edge, drawBatteryRight will place text to the left
  const int batteryX = rect.x + rect.width - 12 - metrics().batteryWidth;
  drawBatteryRight(renderer, Rect{batteryX, rect.y + 5, metrics().batteryWidth, metrics().batteryHeight},
                   showBatteryPercentage);

  int maxTitleWidth = title != nullptr ? renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD) : 0;
  int maxSubtitleWidth =
      subtitle != nullptr ? renderer.getTextWidth(SMALL_FONT_ID, subtitle, EpdFontFamily::REGULAR) : 0;

  // Available space is the distance between the side paddings, and a with side padding between title and subtitle.
  const int availableSpace = rect.width - metrics().contentSidePadding * 3;

  if (maxTitleWidth + maxSubtitleWidth > availableSpace) {
    if ((maxTitleWidth > availableSpace / 2) && (maxSubtitleWidth > availableSpace / 2)) {
      // Both are wider then half the space, truncate both.
      maxTitleWidth = availableSpace / 2;
      maxSubtitleWidth = availableSpace / 2;
    } else {
      // Truncate the the longest one
      if (maxTitleWidth > maxSubtitleWidth) {
        maxTitleWidth = availableSpace - maxSubtitleWidth;
      } else {
        maxSubtitleWidth = availableSpace - maxTitleWidth;
      }
    }
  }

  if (title) {
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);
    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int titleY =
        std::min(rect.y + metrics().batteryBarHeight + 3, rect.y + std::max(0, rect.height - titleLineHeight - 6));
    renderer.drawText(UI_12_FONT_ID, rect.x + metrics().contentSidePadding, titleY, truncatedTitle.c_str(), true,
                      EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width - 1, rect.y + rect.height - 3, 3, true);
  }

  if (subtitle) {
    auto truncatedSubtitle = renderer.truncatedText(SMALL_FONT_ID, subtitle, maxSubtitleWidth, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    const int subtitleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int subtitleY = std::min(rect.y + 50, rect.y + std::max(0, rect.height - subtitleLineHeight - 6));
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - metrics().contentSidePadding - truncatedSubtitleWidth,
                      subtitleY, truncatedSubtitle.c_str(), true);
  }
}

void BaseTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  int currentX = rect.x + metrics().contentSidePadding;
  int rightSpace = metrics().contentSidePadding;
  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - metrics().contentSidePadding - rightLabelWidth, rect.y + 7,
                      truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + hPaddingInSelection;
  }

  auto truncatedLabel = renderer.truncatedText(
      UI_10_FONT_ID, label, rect.width - metrics().contentSidePadding - rightSpace, EpdFontFamily::REGULAR);
  renderer.drawText(UI_10_FONT_ID, currentX, rect.y + 6, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void BaseTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  if (tabBar_ != nullptr && tabBar_->enabled && tabBar_->equalWidth && !tabs.empty()) {
    const int tabCount = static_cast<int>(tabs.size());
    const int tabY = rect.y + 4;
    const int tabHeight = rect.height - 12;
    const auto style = tabBar_->bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    for (int i = 0; i < tabCount; ++i) {
      const int slotX = rect.x + (i * rect.width) / tabCount;
      const int nextSlotX = rect.x + ((i + 1) * rect.width) / tabCount;
      const int slotWidth = nextSlotX - slotX;
      const int inset = std::min(tabBar_->horizontalInset, slotWidth / 2);
      const int tabX = slotX + inset;
      const int tabWidth = std::max(0, slotWidth - inset * 2);
      const auto& tab = tabs[i];
      if (tab.selected && tabBar_->selectionStyle == ThemeMenuSelectionStyle::Fill) {
        renderer.fillRoundedRect(tabX, tabY, tabWidth, tabHeight, tabBar_->selectedCornerRadius,
                                 selected ? Color::Black : Color::LightGray);
      }
      const int textWidth = renderer.getTextWidth(tabBar_->fontId, tab.label, style);
      const int textX = tabX + (tabWidth - textWidth) / 2;
      const int textY = tabY + (tabHeight - renderer.getLineHeight(tabBar_->fontId)) / 2;
      renderer.drawText(tabBar_->fontId, textX, textY, tab.label,
                        !(tab.selected && selected && tabBar_->selectedTextInverted), style);
      if (tab.selected && tabBar_->selectionStyle == ThemeMenuSelectionStyle::Underline) {
        const int underlineY = std::min(rect.y + rect.height - 3, textY + renderer.getLineHeight(tabBar_->fontId) + 3);
        renderer.drawLine(textX, underlineY, textX + textWidth - 1, underlineY, true);
      }
    }
    if (tabBar_->drawDivider) {
      renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
    }
    return;
  }

  int currentX = rect.x + metrics().contentSidePadding;

  if (selected) {
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
  }

  for (const auto& tab : tabs) {
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tab.label, EpdFontFamily::REGULAR);

    if (tab.selected) {
      if (selected) {
        renderer.fillRoundedRect(currentX, rect.y + 1, textWidth + 2 * hPaddingInSelection, rect.height - 4,
                                 cornerRadius, Color::Black);
      } else {
        renderer.fillRectDither(currentX, rect.y, textWidth + 2 * hPaddingInSelection, rect.height - 3,
                                Color::LightGray);
        renderer.drawLine(currentX, rect.y + rect.height - 3, currentX + textWidth + 2 * hPaddingInSelection,
                          rect.y + rect.height - 3, 2, true);
      }
    }

    renderer.drawText(UI_10_FONT_ID, currentX + hPaddingInSelection, rect.y + 6, tab.label, !(tab.selected && selected),
                      EpdFontFamily::REGULAR);

    currentX += textWidth + metrics().tabSpacing + 2 * hPaddingInSelection;
  }

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

int BaseTheme::getListPageItems(int contentHeight, bool hasSubtitle) const {
  int rowHeight = (hasSubtitle) ? metrics().listWithSubtitleRowHeight : metrics().listRowHeight;
  return contentHeight / rowHeight;
}

void BaseTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed) const {
  if (list_ != nullptr && list_->enabled) {
    const ThemeListSpec& spec = *list_;
    const bool hasSubtitle = rowSubtitle != nullptr;
    int rowHeight = hasSubtitle ? metrics().listWithSubtitleRowHeight : metrics().listRowHeight;
    if (hasSubtitle && spec.subtitleRowAutoHeight) {
      rowHeight = spec.subtitleTopPadding + renderer.getLineHeight(spec.fontId) + spec.subtitleInterLineGap +
                  renderer.getLineHeight(spec.subtitleFontId) + spec.subtitleBottomPadding;
    }
    const int rowStep = rowHeight + std::max(0, spec.rowGap);
    const int pageItems = std::max(1, rect.height / std::max(1, rowStep));
    const int totalPages = (itemCount + pageItems - 1) / pageItems;
    const int contentWidth =
        rect.width - (totalPages > 1 ? (metrics().scrollBarWidth + metrics().scrollBarRightOffset) : 1);

    if (totalPages > 1) {
      const int scrollAreaHeight = rect.height;
      const int scrollBarHeight = std::max(metrics().scrollBarWidth, (scrollAreaHeight * pageItems) / itemCount);
      const int currentPage = selectedIndex / pageItems;
      const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
      const int scrollBarX = rect.x + rect.width - metrics().scrollBarRightOffset;
      renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
      renderer.fillRect(scrollBarX - metrics().scrollBarWidth, scrollBarY, metrics().scrollBarWidth, scrollBarHeight,
                        true);
    }

    if (selectedIndex >= 0 && !spec.rowBackgrounds) {
      const int selectedY = rect.y + selectedIndex % pageItems * rowStep;
      Rect selectionRect{rect.x + metrics().contentSidePadding + spec.selectionInsetX, selectedY + spec.selectionInsetY,
                         contentWidth - metrics().contentSidePadding * 2 - spec.selectionInsetX * 2,
                         rowHeight - spec.selectionInsetY * 2};
      if (spec.selectionStyle == ThemeMenuSelectionStyle::Fill && spec.selectionFill) {
        renderer.fillRoundedRect(selectionRect.x, selectionRect.y, selectionRect.width, selectionRect.height,
                                 spec.selectionCornerRadius, Color::LightGray);
      }
      if (spec.selectionStyle == ThemeMenuSelectionStyle::Outline || spec.selectionOutline) {
        renderer.drawRoundedRect(selectionRect.x, selectionRect.y, selectionRect.width, selectionRect.height, 1,
                                 spec.selectionCornerRadius, true);
      }
    }

    const int rowX = rect.x + spec.rowSidePadding;
    const int rowWidth = contentWidth - spec.rowSidePadding * 2;
    int textX =
        spec.rowBackgrounds ? rowX + spec.textInsetX : rect.x + metrics().contentSidePadding + hPaddingInSelection;
    int textWidth = spec.rowBackgrounds ? rowWidth - spec.textInsetX * 2
                                        : contentWidth - metrics().contentSidePadding * 2 - hPaddingInSelection * 2;
    const int iconSize =
        spec.iconSize > 0 ? spec.iconSize : ((rowSubtitle != nullptr) ? mainMenuIconSize : listIconSize);
    if (rowIcon != nullptr && spec.showIcons) {
      textX += iconSize + spec.textGap;
      textWidth -= iconSize + spec.textGap;
    }

    const auto pageStartIndex = selectedIndex / pageItems * pageItems;
    const auto titleStyle = spec.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
      const int itemY = rect.y + (i % pageItems) * rowStep;
      const bool selected = i == selectedIndex;
      int rowTextWidth = textWidth;
      if (spec.rowBackgrounds) {
        renderer.fillRoundedRect(rowX, itemY, rowWidth, rowHeight, spec.selectionCornerRadius,
                                 selected ? Color::Black : Color::White);
      }

      int valueWidth = 0;
      std::string valueText;
      if (rowValue != nullptr) {
        valueText = rowValue(i);
        valueText = renderer.truncatedText(spec.valueFontId, valueText.c_str(), maxListValueWidth);
        valueWidth = renderer.getTextWidth(spec.valueFontId, valueText.c_str()) + hPaddingInSelection;
        rowTextWidth -= valueWidth;
      }

      const auto itemName = rowTitle(i);
      const auto item = renderer.truncatedText(spec.fontId, itemName.c_str(), rowTextWidth, titleStyle);
      bool centerSingleLine = spec.centerSingleLineRows && rowSubtitle == nullptr;
      std::string subtitleText;
      if (rowSubtitle != nullptr) {
        subtitleText = rowSubtitle(i);
        centerSingleLine = spec.centerSingleLineRows && subtitleText.empty();
      }
      const int titleY = centerSingleLine
                             ? itemY + (rowHeight - renderer.getLineHeight(spec.fontId)) / 2
                             : itemY + (spec.subtitleRowAutoHeight && rowSubtitle != nullptr ? spec.subtitleTopPadding
                                                                                             : spec.titleOffsetY);
      renderer.drawText(spec.fontId, textX, titleY, item.c_str(), !(selected && spec.selectedTextInverted), titleStyle);
      if (selected && spec.selectionStyle == ThemeMenuSelectionStyle::Underline) {
        const int underlineWidth = renderer.getTextWidth(spec.fontId, item.c_str(), titleStyle);
        const int underlineY = std::min(itemY + rowHeight - 4, titleY + renderer.getLineHeight(spec.fontId) + 2);
        renderer.drawLine(textX, underlineY, textX + underlineWidth - 1, underlineY, 1, true);
      }

      if (rowDimmed && rowDimmed(i) && !selected) {
        const int titleWidth = renderer.getTextWidth(spec.fontId, item.c_str(), titleStyle);
        const int lineH = renderer.getLineHeight(spec.fontId);
        for (int py = titleY; py < titleY + lineH; py++)
          for (int px = textX; px < textX + titleWidth; px++)
            if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
      }

      if (rowIcon != nullptr && spec.showIcons) {
        const UIIcon icon = rowIcon(i);
        const int titleLineHeight = renderer.getLineHeight(spec.fontId);
        const int textBlockTop = spec.titleOffsetY;
        const int textBlockBottom = rowSubtitle != nullptr
                                        ? spec.subtitleOffsetY + renderer.getLineHeight(spec.subtitleFontId)
                                        : spec.titleOffsetY + titleLineHeight;
        const int iconY = itemY + (textBlockTop + textBlockBottom - iconSize) / 2 + spec.iconOffsetY;
        const int iconX =
            spec.rowBackgrounds ? rowX + spec.textInsetX : rect.x + metrics().contentSidePadding + hPaddingInSelection;
        if (!drawThemeIcon(renderer, icon, iconX, iconY, iconSize)) {
          const uint8_t* iconBitmap = iconForName(icon, iconSize);
          if (iconBitmap != nullptr) {
            renderer.drawIcon(iconBitmap, iconX, iconY, iconSize, iconSize);
          }
        }
      }

      if (rowSubtitle != nullptr && !subtitleText.empty()) {
        const auto subtitle = renderer.truncatedText(spec.subtitleFontId, subtitleText.c_str(), rowTextWidth);
        const int subtitleY = spec.subtitleRowAutoHeight
                                  ? titleY + renderer.getLineHeight(spec.fontId) + spec.subtitleInterLineGap
                                  : itemY + spec.subtitleOffsetY;
        renderer.drawText(spec.subtitleFontId, textX, subtitleY, subtitle.c_str(),
                          !(selected && spec.selectedTextInverted));
      }

      if (!valueText.empty()) {
        if (selected && highlightValue) {
          renderer.fillRoundedRect(
              rect.x + contentWidth - metrics().contentSidePadding - hPaddingInSelection - valueWidth, itemY,
              valueWidth + hPaddingInSelection, rowHeight, spec.selectionCornerRadius, Color::Black);
        }
        const int valueY = (centerSingleLine || spec.centerValueVertically)
                               ? itemY + (rowHeight - renderer.getLineHeight(spec.valueFontId)) / 2
                               : itemY + (rowSubtitle != nullptr ? spec.subtitleValueOffsetY : spec.valueOffsetY);
        const int valueX = spec.rowBackgrounds ? rowX + rowWidth - spec.textInsetX - valueWidth
                                               : rect.x + contentWidth - metrics().contentSidePadding - valueWidth;
        renderer.drawText(spec.valueFontId, valueX, valueY, valueText.c_str(),
                          !(selected && (highlightValue || (spec.rowBackgrounds && spec.selectedTextInverted))));
      }
    }
    return;
  }

  int rowHeight = (rowSubtitle != nullptr) ? metrics().listWithSubtitleRowHeight : metrics().listRowHeight;
  int pageItems = rect.height / rowHeight;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;

    // Draw scroll bar
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - metrics().scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - metrics().scrollBarWidth, scrollBarY, metrics().scrollBarWidth, scrollBarHeight,
                      true);
  }

  // Draw selection
  int contentWidth = rect.width - (totalPages > 1 ? (metrics().scrollBarWidth + metrics().scrollBarRightOffset) : 1);
  if (selectedIndex >= 0) {
    renderer.fillRoundedRect(rect.x + metrics().contentSidePadding, rect.y + selectedIndex % pageItems * rowHeight,
                             contentWidth - metrics().contentSidePadding * 2, rowHeight, cornerRadius,
                             Color::LightGray);
  }

  int textX = rect.x + metrics().contentSidePadding + hPaddingInSelection;
  int textWidth = contentWidth - metrics().contentSidePadding * 2 - hPaddingInSelection * 2;
  int iconSize;
  if (rowIcon != nullptr) {
    iconSize = (rowSubtitle != nullptr) ? mainMenuIconSize : listIconSize;
    textX += iconSize + hPaddingInSelection;
    textWidth -= iconSize + hPaddingInSelection;
  }

  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    int rowTextWidth = textWidth;

    // Draw name
    int valueWidth = 0;
    std::string valueText = "";
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), maxListValueWidth);
      valueWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str()) + hPaddingInSelection;
      rowTextWidth -= valueWidth;
    }

    auto itemName = rowTitle(i);
    auto item = renderer.truncatedText(UI_10_FONT_ID, itemName.c_str(), rowTextWidth);
    renderer.drawText(UI_10_FONT_ID, textX, itemY + 7, item.c_str(), true);

    // Apply checkerboard dither to create gray text effect for dimmed items
    if (rowDimmed && rowDimmed(i) && i != selectedIndex) {
      const int titleWidth = renderer.getTextWidth(UI_10_FONT_ID, item.c_str());
      const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
      for (int py = itemY + 7; py < itemY + 7 + lineH; py++)
        for (int px = textX; px < textX + titleWidth; px++)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      const int textBlockTop = 7;
      const int textBlockBottom =
          rowSubtitle != nullptr ? 30 + renderer.getLineHeight(SMALL_FONT_ID) : textBlockTop + titleLineHeight;
      const int iconY = itemY + (textBlockTop + textBlockBottom - iconSize) / 2;
      if (!drawThemeIcon(renderer, icon, rect.x + metrics().contentSidePadding + hPaddingInSelection, iconY,
                         iconSize)) {
        const uint8_t* iconBitmap = iconForName(icon, iconSize);
        if (iconBitmap != nullptr) {
          renderer.drawIcon(iconBitmap, rect.x + metrics().contentSidePadding + hPaddingInSelection, iconY, iconSize,
                            iconSize);
        }
      }
    }

    if (rowSubtitle != nullptr) {
      // Draw subtitle
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
      renderer.drawText(SMALL_FONT_ID, textX, itemY + 30, subtitle.c_str(), true);
    }

    // Draw value
    if (!valueText.empty()) {
      if (i == selectedIndex && highlightValue) {
        renderer.fillRoundedRect(
            rect.x + contentWidth - metrics().contentSidePadding - hPaddingInSelection - valueWidth, itemY,
            valueWidth + hPaddingInSelection, rowHeight, cornerRadius, Color::Black);
      }

      int valueY = itemY + 6;
      if (rowSubtitle != nullptr) {
        valueY = itemY + 16;
      }
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - metrics().contentSidePadding - valueWidth, valueY,
                        valueText.c_str(), !(i == selectedIndex && highlightValue));
    }
  }
}

void BaseTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  const int pageWidth = renderer.getScreenWidth();
  const int buttonWidth = buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->buttonWidth : 80;
  const int smallButtonHeight = buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->smallButtonHeight : 15;
  const int buttonHeight = metrics().buttonHintsHeight;
  const int buttonY = metrics().buttonHintsHeight;  // Distance from bottom
  const int textYOffset = buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->textOffsetY : 7;
  const int buttonCornerRadius =
      buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->cornerRadius : cornerRadius;
  const int fontId = buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->fontId : SMALL_FONT_ID;
  const auto style = buttonHints_ != nullptr && buttonHints_->enabled && buttonHints_->bold ? EpdFontFamily::BOLD
                                                                                            : EpdFontFamily::REGULAR;
  const ThemeButtonHintsStyle hintStyle =
      buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->style : ThemeButtonHintsStyle::Buttons;
  const bool shapes = hintStyle == ThemeButtonHintsStyle::Shapes;
  const int shapeSize = buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->shapeSize : 18;
  if (hintStyle == ThemeButtonHintsStyle::Groups) {
    const int sidePadding = buttonHints_->sidePadding;
    const int groupGap = buttonHints_->groupGap;
    const int bottomMargin = buttonHints_->bottomMargin;
    const int innerPadding = buttonHints_->innerPadding;
    const int hintHeight = std::max(1, metrics().buttonHintsHeight - bottomMargin);
    const int groupWidth = std::max(1, (pageWidth - sidePadding * 2 - groupGap) / 2);
    const int outlineY = pageHeight - hintHeight - bottomMargin;
    const int leftGroupX = sidePadding;
    const int rightGroupX = leftGroupX + groupWidth + groupGap;
    const char* labels[] = {btn1, btn2, btn3, btn4};
    const int selectWidth = labels[1] != nullptr ? renderer.getTextWidth(fontId, labels[1], style) : 0;
    const int downWidth = labels[3] != nullptr ? renderer.getTextWidth(fontId, labels[3], style) : 0;

    renderer.fillRect(leftGroupX, outlineY, groupWidth, hintHeight, false);
    renderer.fillRect(rightGroupX, outlineY, groupWidth, hintHeight, false);
    renderer.drawRoundedRect(leftGroupX, outlineY, groupWidth, hintHeight, 2, buttonCornerRadius, true);
    renderer.drawRoundedRect(rightGroupX, outlineY, groupWidth, hintHeight, 2, buttonCornerRadius, true);

    const int textY = outlineY + (hintHeight - renderer.getLineHeight(fontId)) / 2;
    if (labels[0] != nullptr && labels[0][0] != '\0') {
      renderer.drawText(fontId, leftGroupX + innerPadding, textY, labels[0], true, style);
    }
    if (labels[1] != nullptr && labels[1][0] != '\0') {
      renderer.drawText(fontId, leftGroupX + groupWidth - innerPadding - selectWidth, textY, labels[1], true, style);
    }
    if (labels[2] != nullptr && labels[2][0] != '\0') {
      renderer.drawText(fontId, rightGroupX + innerPadding, textY, labels[2], true, style);
    }
    if (labels[3] != nullptr && labels[3][0] != '\0') {
      renderer.drawText(fontId, rightGroupX + groupWidth - innerPadding - downWidth, textY, labels[3], true, style);
    }
    renderer.setOrientation(orig_orientation);
    return;
  }
  // X3 has wider screen in portrait (528 vs 480), use more spacing
  constexpr int x4ButtonPositions[] = {58, 146, 254, 342};
  constexpr int x3ButtonPositions[] = {65, 157, 291, 383};
  const int* buttonPositions = gpio.deviceIsX3() ? x3ButtonPositions : x4ButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    const int x = buttonPositions[i];
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      if (shapes) {
        drawButtonHintShape(renderer, shapeForButtonHintLabel(labels[i]), x + buttonWidth / 2,
                            pageHeight - buttonY + buttonHeight / 2, shapeSize);
        continue;
      }
      if (buttonHints_ == nullptr || !buttonHints_->enabled || buttonHints_->fill) {
        renderer.fillRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, buttonCornerRadius, Color::White);
      }
      if (buttonHints_ == nullptr || !buttonHints_->enabled || buttonHints_->outline) {
        renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, buttonCornerRadius, true, true,
                                 false, false, true);
      }
      const int textWidth = renderer.getTextWidth(fontId, labels[i], style);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(fontId, textX, pageHeight - buttonY + textYOffset, labels[i], true, style);
    } else if (buttonHints_ == nullptr || !buttonHints_->enabled || buttonHints_->drawEmpty) {
      if (buttonHints_ == nullptr || !buttonHints_->enabled || buttonHints_->fill) {
        renderer.fillRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, buttonCornerRadius,
                                 Color::White);
      }
      if (buttonHints_ == nullptr || !buttonHints_->enabled || buttonHints_->outline) {
        renderer.drawRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, 1,
                                 buttonCornerRadius, true, true, false, false, true);
      }
    }
  }

  renderer.setOrientation(orig_orientation);
}

void BaseTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  const int screenWidth = renderer.getScreenWidth();
  const int buttonWidth = metrics().sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 78;                         // Height on screen (width when rotated)
  constexpr int buttonMargin = 0;
  const int buttonCornerRadius =
      buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->cornerRadius : cornerRadius;
  const int fontId = buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->fontId : SMALL_FONT_ID;
  const auto style = buttonHints_ != nullptr && buttonHints_->enabled && buttonHints_->bold ? EpdFontFamily::BOLD
                                                                                            : EpdFontFamily::REGULAR;
  const ThemeButtonHintsStyle hintStyle =
      buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->style : ThemeButtonHintsStyle::Buttons;
  const bool shapes = hintStyle == ThemeButtonHintsStyle::Shapes;
  const int shapeSize = buttonHints_ != nullptr && buttonHints_->enabled ? buttonHints_->shapeSize : 18;

  auto drawSideHint = [&](const int x, const int y, const char* label, const bool leftOpen, const bool rightOpen) {
    if (label == nullptr || label[0] == '\0') return;

    if (shapes) {
      ButtonHintShape shape = shapeForButtonHintLabel(label);
      if (strcmp(label, ">") == 0 || matchesLabel(label, tr(STR_DIR_RIGHT))) {
        shape = ButtonHintShape::Up;
      } else if (strcmp(label, "<") == 0 || matchesLabel(label, tr(STR_DIR_LEFT))) {
        shape = ButtonHintShape::Down;
      }
      drawButtonHintShape(renderer, shape, x + buttonWidth / 2, y + buttonHeight / 2, shapeSize);
      return;
    }

    if (buttonHints_ == nullptr || !buttonHints_->enabled || buttonHints_->fill) {
      renderer.fillRoundedRect(x, y, buttonWidth, buttonHeight, buttonCornerRadius, Color::White);
    }
    if (buttonHints_ == nullptr || !buttonHints_->enabled || buttonHints_->outline) {
      renderer.drawRoundedRect(x, y, buttonWidth, buttonHeight, 1, buttonCornerRadius, leftOpen, rightOpen, leftOpen,
                               rightOpen, true);
    }
    const int textWidth = renderer.getTextWidth(fontId, label, style);
    const int textHeight = renderer.getTextHeight(fontId);
    const int textX = x + (buttonWidth - textHeight) / 2;
    renderer.drawTextRotated90CW(fontId, textX, y + (buttonHeight + textWidth) / 2, label, true, style);
  };

  if (gpio.deviceIsX3()) {
    // X3 layout: Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    drawSideHint(buttonMargin, x3ButtonY, topBtn, false, true);
    drawSideHint(screenWidth - buttonWidth, x3ButtonY, bottomBtn, true, false);
  } else {
    // X4 layout: Both buttons stacked on right side
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonWidth;

    for (int i = 0; i < 2; i++) {
      const int y = topHintButtonY + (i * buttonHeight) + 5;
      drawSideHint(x, y, labels[i], true, false);
    }
  }
}

void BaseTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer,
                                    bool coverStripSelected) const {
  if (homeRecents_ != nullptr && homeRecents_->type == ThemeHomeRecentsType::CoverStrip) {
    drawCoverStripRecents(renderer, rect, recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          storeCoverBuffer, coverStripSelected);
    return;
  }
  if (homeRecents_ != nullptr && homeRecents_->type == ThemeHomeRecentsType::Card) {
    drawCardRecents(renderer, rect, recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                    storeCoverBuffer);
    return;
  }
  if (homeRecents_ != nullptr && homeRecents_->type == ThemeHomeRecentsType::None) {
    coverBufferStored = false;
    coverRendered = false;
    bufferRestored = false;
    return;
  }

  const int tileWidth = rect.width - 2 * metrics().contentSidePadding;
  const int tileHeight = rect.height;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();
  if (coverWidth == 0) {
    coverWidth = metrics().homeCoverHeight * 0.6;
  }

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    RecentBook book = recentBooks[0];
    if (!coverRendered) {
      std::string coverPath = book.coverBmpPath;
      bool hasCover = true;
      int tileX = metrics().contentSidePadding;
      if (coverPath.empty()) {
        hasCover = false;
      } else {
        const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, metrics().homeCoverHeight);

        // First time: load cover from SD and render
        HalFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            coverWidth = bitmap.getWidth();
            renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,
                                metrics().homeCoverHeight);
          } else {
            hasCover = false;
          }
          file.close();
        }
      }

      // Draw either way
      renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth, metrics().homeCoverHeight,
                        true);

      if (!hasCover) {
        // Render empty cover
        renderer.fillRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection + (metrics().homeCoverHeight / 3),
                          coverWidth, 2 * metrics().homeCoverHeight / 3, true);
        renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24, 32, 32);
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    bool bookSelected = (selectorIndex == 0);

    int tileX = metrics().contentSidePadding;
    int textWidth = tileWidth - 2 * hPaddingInSelection - metrics().verticalSpacing - coverWidth;

    if (bookSelected) {
      // Draw selection box
      renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                               Color::LightGray);
      renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection, metrics().homeCoverHeight,
                              Color::LightGray);
      renderer.fillRectDither(tileX + hPaddingInSelection + coverWidth, tileY + hPaddingInSelection,
                              tileWidth - hPaddingInSelection - coverWidth, metrics().homeCoverHeight,
                              Color::LightGray);
      renderer.fillRoundedRect(tileX, tileY + metrics().homeCoverHeight + hPaddingInSelection, tileWidth,
                               hPaddingInSelection, cornerRadius, false, false, true, true, Color::LightGray);
    }

    auto titleLines = renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), textWidth, 3, EpdFontFamily::BOLD);

    auto author = renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), textWidth);
    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int titleBlockHeight = titleLineHeight * static_cast<int>(titleLines.size());
    const int authorHeight = book.author.empty() ? 0 : (renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2);
    const int totalBlockHeight = titleBlockHeight + authorHeight;
    int titleY = tileY + tileHeight / 2 - totalBlockHeight / 2;
    const int textX = tileX + hPaddingInSelection + coverWidth + metrics().verticalSpacing;
    for (const auto& line : titleLines) {
      renderer.drawText(UI_12_FONT_ID, textX, titleY, line.c_str(), true, EpdFontFamily::BOLD);
      titleY += titleLineHeight;
    }
    if (!book.author.empty()) {
      titleY += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, titleY, author.c_str(), true);
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}

void BaseTheme::drawCoverStripRecents(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                      const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                      bool bufferRestored, std::function<bool()> storeCoverBuffer,
                                      bool coverStripSelected) const {
  if (bufferRestored && coverRendered) {
    return;
  }

  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    coverBufferStored = false;
    coverRendered = false;
    return;
  }

  const ThemeHomeRecentsSpec& spec = *homeRecents_;
  if (spec.slots.empty()) {
    coverBufferStored = false;
    coverRendered = false;
    return;
  }

  const int bookCount = static_cast<int>(recentBooks.size());
  const int selected = selectorIndex >= 0 && selectorIndex < bookCount ? selectorIndex : 0;
  const auto& m = metrics();

  if (spec.drawPanel) {
    const int inset = std::max(0, spec.panelInsetX);
    renderer.fillRoundedRect(rect.x + inset, rect.y, std::max(0, rect.width - inset * 2), rect.height,
                             spec.panelCornerRadius, Color::LightGray);
  }

  auto resolveBookIndex = [&](const ThemeCoverSlotSpec& slot) {
    switch (slot.book) {
      case ThemeBookRef::Previous:
        if (bookCount <= 1) return -1;
        return spec.wrap ? (selected + bookCount - 1) % bookCount : selected - 1;
      case ThemeBookRef::Next:
        if (bookCount <= 1) return -1;
        return spec.wrap ? (selected + 1) % bookCount : selected + 1;
      case ThemeBookRef::Index:
        return slot.bookIndex >= 0 && slot.bookIndex < bookCount ? slot.bookIndex : -1;
      case ThemeBookRef::Selected:
      default:
        return selected;
    }
  };

  auto drawCover = [&](int bookIndex, int x, int y, int w, int h, bool selectedCover) {
    bool hasCover = bookIndex >= 0 && bookIndex < bookCount && !recentBooks[bookIndex].coverBmpPath.empty();
    if (hasCover) {
      auto drawThumb = [&](int thumbHeight) {
        const std::string coverBmpPath = UITheme::getCoverThumbPath(recentBooks[bookIndex].coverBmpPath, thumbHeight);
        HalFile file;
        if (!Storage.openFileForRead("HOME", coverBmpPath, file)) return false;
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          float cropX = 0.0f;
          float cropY = 0.0f;
          const float bitmapAspect = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
          const float targetAspect = static_cast<float>(w) / static_cast<float>(h);
          if (bitmapAspect > targetAspect) {
            cropX = std::max(0.0f, 1.0f - targetAspect / bitmapAspect);
          } else if (bitmapAspect < targetAspect) {
            cropY = std::max(0.0f, 1.0f - bitmapAspect / targetAspect);
          }
          renderer.drawBitmap(bitmap, x, y, w, h, cropX, cropY);
          return true;
        } else {
          return false;
        }
      };

      bool drawn = drawThumb(h);
      if (!drawn && h != m.homeCoverHeight) {
        drawn = drawThumb(m.homeCoverHeight);
      }
      if (!drawn) {
        hasCover = false;
      }
    }

    if (!hasCover) {
      renderer.drawRect(x, y, w, h, true);
      renderer.fillRect(x, y + h / 3, w, 2 * h / 3, true);
      renderer.drawIcon(CoverIcon, x + std::max(4, (w - 32) / 2), y + 16, 32, 32);
    }

    renderer.drawRect(x, y, w, h, true);
    if (selectedCover) {
      const int inactiveWidth =
          spec.inactiveSelectionLineWidth > 0 ? spec.inactiveSelectionLineWidth : spec.selectionLineWidth;
      const int lineWidth = std::max(1, coverStripSelected ? spec.selectionLineWidth : inactiveWidth);
      for (int i = 0; i < lineWidth; ++i) {
        renderer.drawRoundedRect(x - 6 - i, y - 6 - i, w + 12 + 2 * i, h + 12 + 2 * i, lineWidth,
                                 spec.selectionCornerRadius, true);
      }
    }
  };

  for (const auto& slot : spec.slots) {
    const int bookIndex = resolveBookIndex(slot);
    if (bookIndex < 0 || bookIndex >= bookCount) continue;

    const int h = std::min(slot.height, rect.height);
    const int w = std::max(1, h * std::max(1, slot.widthPercent) / 100);
    int x = rect.x + (rect.width - w) / 2;
    if (slot.x == ThemeSlotX::Padding) {
      x = rect.x + m.contentSidePadding;
    } else if (slot.x == ThemeSlotX::RightPadding) {
      x = rect.x + rect.width - m.contentSidePadding - w;
    }
    x += slot.xOffset;

    int y = rect.y;
    if (slot.y == ThemeSlotY::Center) {
      y = rect.y + (rect.height - h) / 2;
    }
    y += slot.yOffset;

    const bool selectedCover = slot.selected && (slot.book != ThemeBookRef::Index || bookIndex == selected);
    drawCover(bookIndex, x, y, w, h, selectedCover);

    if (slot.title.enabled) {
      const int maxWidth = std::max(40, w + 28);
      const auto style = slot.title.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      const auto titleLines = renderer.wrappedText(slot.title.fontId, recentBooks[bookIndex].title.c_str(), maxWidth,
                                                   slot.title.maxLines, style);
      int titleY = y + h + slot.title.offsetY;
      for (const auto& line : titleLines) {
        const int textWidth = renderer.getTextWidth(slot.title.fontId, line.c_str(), style);
        renderer.drawText(slot.title.fontId, x + (w - textWidth) / 2, titleY, line.c_str(), true, style);
        titleY += renderer.getLineHeight(slot.title.fontId);
      }
    }
  }

  coverBufferStored = storeCoverBuffer();
  coverRendered = coverBufferStored;
}

void BaseTheme::drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const {
  constexpr int padding = 48;
  renderer.drawText(UI_12_FONT_ID, rect.x + padding,
                    rect.y + rect.height / 2 - renderer.getLineHeight(UI_12_FONT_ID) - 2, tr(STR_NO_OPEN_BOOK), true,
                    EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, rect.x + padding, rect.y + rect.height / 2 + 2, tr(STR_START_READING), true);
}

void BaseTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  if (buttonMenu_ != nullptr && buttonMenu_->enabled) {
    const auto& spec = *buttonMenu_;
    const auto& m = metrics();
    const int panelWidth = spec.panelWidth > 0 ? std::min(spec.panelWidth, rect.width) : rect.width;
    const int panelX = rect.x + (rect.width - panelWidth) / 2;
    const int panelHeight = buttonCount * m.menuRowHeight + std::max(0, buttonCount - 1) * m.menuSpacing;
    const int panelY =
        spec.centerVertically && panelHeight < rect.height ? rect.y + (rect.height - panelHeight) / 2 : rect.y;

    if (spec.drawPanel) {
      renderer.drawRoundedRect(panelX, panelY, panelWidth, panelHeight, 1, spec.panelCornerRadius, true);
    }

    for (int i = 0; i < buttonCount; ++i) {
      std::string labelStr = buttonLabel(i);
      const char* label = labelStr.c_str();
      const auto style = spec.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      Rect tileRect = Rect{panelX + spec.selectionInset, panelY + i * (m.menuRowHeight + m.menuSpacing),
                           panelWidth - spec.selectionInset * 2, m.menuRowHeight};
      if (spec.selectionStyle == ThemeMenuSelectionStyle::Pill) {
        const int maxLabelWidth = std::max(0, panelWidth - spec.selectionInset * 2 - spec.rowPaddingX);
        labelStr = renderer.truncatedText(spec.fontId, label, maxLabelWidth, style);
        label = labelStr.c_str();
        tileRect.width = std::min(tileRect.width, renderer.getTextWidth(spec.fontId, label, style) + spec.rowPaddingX);
      }
      const bool selected = selectedIndex == i;

      if (spec.selectionStyle == ThemeMenuSelectionStyle::Pill) {
        renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, spec.selectionCornerRadius,
                                 selected ? Color::Black : Color::White);
      } else if (selected) {
        if (spec.selectionStyle == ThemeMenuSelectionStyle::Outline) {
          renderer.drawRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, 1,
                                   spec.selectionCornerRadius, true);
        } else if (spec.selectionStyle == ThemeMenuSelectionStyle::Triangle) {
          constexpr int triangleWidth = 12;
          constexpr int triangleHeight = 18;
          const int triangleX = panelX + spec.selectionInset;
          const int triangleCenterY = tileRect.y + tileRect.height / 2;
          const int triangleXPoints[3] = {triangleX, triangleX, triangleX + triangleWidth};
          const int triangleYPoints[3] = {triangleCenterY - triangleHeight / 2, triangleCenterY + triangleHeight / 2,
                                          triangleCenterY};
          renderer.fillPolygon(triangleXPoints, triangleYPoints, 3, true);
        } else if (spec.selectionStyle == ThemeMenuSelectionStyle::Underline) {
          // Drawn after text layout below.
        } else {
          renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, spec.selectionCornerRadius,
                                   spec.selectionFillBlack ? Color::Black : Color::LightGray);
        }
      }

      const int lineHeight = renderer.getLineHeight(spec.fontId);
      const int textY = tileRect.y + (tileRect.height - lineHeight) / 2;
      int textX = tileRect.x + spec.textInsetX;

      if (spec.showIcons && rowIcon != nullptr) {
        UIIcon icon = rowIcon(i);
        const int iconY = tileRect.y + (tileRect.height - mainMenuIconSize) / 2;
        if (!drawThemeIcon(renderer, icon, textX, iconY, mainMenuIconSize)) {
          const uint8_t* iconBitmap = iconForName(icon, mainMenuIconSize);
          if (iconBitmap != nullptr) {
            renderer.drawIcon(iconBitmap, textX, iconY, mainMenuIconSize, mainMenuIconSize);
          }
        }
        if (hasThemeIcon(icon) || iconForName(icon, mainMenuIconSize) != nullptr) {
          textX += mainMenuIconSize + hPaddingInSelection + 2;
        }
      }

      if (spec.centeredText) {
        const int textWidth = renderer.getTextWidth(spec.fontId, label, style);
        textX = tileRect.x + (tileRect.width - textWidth) / 2;
      }
      renderer.drawText(
          spec.fontId, textX, textY, label,
          !(selected && (spec.selectedTextInverted || spec.selectionStyle == ThemeMenuSelectionStyle::Pill)), style);
      if (selected && spec.selectionStyle == ThemeMenuSelectionStyle::Underline) {
        const int textWidth = renderer.getTextWidth(spec.fontId, label, style);
        const int underlineY = std::min(tileRect.y + tileRect.height - 5, textY + lineHeight + 2);
        renderer.drawLine(textX, underlineY, textX + textWidth - 1, underlineY, 1, true);
      }
    }
    return;
  }

  for (int i = 0; i < buttonCount; ++i) {
    int tileWidth = rect.width - metrics().contentSidePadding * 2;
    Rect tileRect =
        Rect{rect.x + metrics().contentSidePadding, rect.y + i * (metrics().menuRowHeight + metrics().menuSpacing),
             tileWidth, metrics().menuRowHeight};

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, cornerRadius, Color::LightGray);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    int textX = tileRect.x + 16;
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = tileRect.y + (metrics().menuRowHeight - lineHeight) / 2;

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const int iconY = tileRect.y + (tileRect.height - mainMenuIconSize) / 2;
      if (!drawThemeIcon(renderer, icon, textX, iconY, mainMenuIconSize)) {
        const uint8_t* iconBitmap = iconForName(icon, mainMenuIconSize);
        if (iconBitmap != nullptr) {
          renderer.drawIcon(iconBitmap, textX, iconY, mainMenuIconSize, mainMenuIconSize);
        }
      }
      if (hasThemeIcon(icon) || iconForName(icon, mainMenuIconSize) != nullptr) {
        textX += mainMenuIconSize + hPaddingInSelection + 2;
      }
    }

    renderer.drawText(UI_12_FONT_ID, textX, textY, label, true);
  }
}

void BaseTheme::drawCardRecents(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                const bool bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const bool hasContinueReading = !recentBooks.empty();
  const bool bookSelected = hasContinueReading && selectorIndex == 0;

  // --- Top "book" card for the current title (selectorIndex == 0) ---
  // When there's no cover image, use fixed size (half screen)
  // When there's cover image, adapt width to image aspect ratio, keep height fixed at 400px
  const int baseHeight = rect.height;  // Fixed height (400px)

  int bookWidth, bookX;
  bool hasCoverImage = false;

  if (hasContinueReading && !recentBooks[0].coverBmpPath.empty()) {
    // Try to get actual image dimensions from BMP header
    const std::string coverBmpPath =
        UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, metrics().homeCoverHeight);

    HalFile file;
    if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        hasCoverImage = true;
        const int imgWidth = bitmap.getWidth();
        const int imgHeight = bitmap.getHeight();

        // Calculate width based on aspect ratio, maintaining baseHeight
        if (imgWidth > 0 && imgHeight > 0) {
          const float aspectRatio = static_cast<float>(imgWidth) / static_cast<float>(imgHeight);
          bookWidth = static_cast<int>(baseHeight * aspectRatio);

          // Ensure width doesn't exceed reasonable limits (max 90% of screen width)
          const int maxWidth = static_cast<int>(rect.width * 0.9f);
          if (bookWidth > maxWidth) {
            bookWidth = maxWidth;
          }
        } else {
          bookWidth = rect.width / 2;  // Fallback
        }
      }
    }
  }

  if (!hasCoverImage) {
    // No cover: use half screen size
    bookWidth = rect.width / 2;
  }

  bookX = rect.x + (rect.width - bookWidth) / 2;
  const int bookY = rect.y;
  const int bookHeight = baseHeight;

  // Bookmark dimensions (used in multiple places)
  const int bookmarkWidth = bookWidth / 8;
  const int bookmarkHeight = bookHeight / 5;
  const int bookmarkX = bookX + bookWidth - bookmarkWidth - 10;
  const int bookmarkY = bookY + 5;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  {
    // Draw cover image as background if available (inside the box)
    // Only load from SD on first render, then use stored buffer

    if (hasContinueReading && !recentBooks[0].coverBmpPath.empty() && !coverRendered) {
      const std::string coverBmpPath =
          UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, metrics().homeCoverHeight);

      // First time: load cover from SD and render
      HalFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          LOG_DBG("THEME", "Rendering bmp");

          // Draw the cover image (bookWidth and bookHeight already match image aspect ratio)
          renderer.drawBitmap(bitmap, bookX, bookY, bookWidth, bookHeight);

          // Draw border around the card
          renderer.drawRect(bookX, bookY, bookWidth, bookHeight);

          // No bookmark ribbon when cover is shown - it would just cover the art

          // Store the buffer with cover image for fast navigation
          coverBufferStored = storeCoverBuffer();
          coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer

          // First render: if selected, draw selection indicators now
          if (bookSelected) {
            LOG_DBG("THEME", "Drawing selection");
            renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
            renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
          }
        }
      }
    }

    if (!bufferRestored && !coverRendered) {
      // No cover image: draw border or fill, plus bookmark as visual flair
      if (bookSelected) {
        renderer.fillRect(bookX, bookY, bookWidth, bookHeight);
      } else {
        renderer.drawRect(bookX, bookY, bookWidth, bookHeight);
      }

      // Draw bookmark ribbon when no cover image (visual decoration)
      if (hasContinueReading) {
        const int notchDepth = bookmarkHeight / 3;
        const int centerX = bookmarkX + bookmarkWidth / 2;

        const int xPoints[5] = {
            bookmarkX,                  // top-left
            bookmarkX + bookmarkWidth,  // top-right
            bookmarkX + bookmarkWidth,  // bottom-right
            centerX,                    // center notch point
            bookmarkX                   // bottom-left
        };
        const int yPoints[5] = {
            bookmarkY,                                // top-left
            bookmarkY,                                // top-right
            bookmarkY + bookmarkHeight,               // bottom-right
            bookmarkY + bookmarkHeight - notchDepth,  // center notch point
            bookmarkY + bookmarkHeight                // bottom-left
        };

        // Draw bookmark ribbon (inverted if selected)
        renderer.fillPolygon(xPoints, yPoints, 5, !bookSelected);
      }
    }

    // If buffer was restored, draw selection indicators if needed
    if (bufferRestored && bookSelected && coverRendered) {
      // Draw selection border (no bookmark inversion needed since cover has no bookmark)
      renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
      renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
    } else if (!coverRendered && !bufferRestored) {
      // Selection border already handled above in the no-cover case
    }
  }

  if (hasContinueReading) {
    const std::string& lastBookTitle = recentBooks[0].title;
    const std::string& lastBookAuthor = recentBooks[0].author;

    // Invert text colors based on selection state:
    // - With cover: selected = white text on black box, unselected = black text on white box
    // - Without cover: selected = white text on black card, unselected = black text on white card

    auto lines = renderer.wrappedText(UI_12_FONT_ID, lastBookTitle.c_str(), bookWidth - 40, 3);

    // Book title text
    int totalTextHeight = renderer.getLineHeight(UI_12_FONT_ID) * static_cast<int>(lines.size());
    if (!lastBookAuthor.empty()) {
      totalTextHeight += renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    }

    // Vertically center the title block within the card
    int titleYStart = bookY + (bookHeight - totalTextHeight) / 2;

    const auto truncatedAuthor = lastBookAuthor.empty()
                                     ? std::string{}
                                     : renderer.truncatedText(UI_10_FONT_ID, lastBookAuthor.c_str(), bookWidth - 40);

    // If cover image was rendered, draw box behind title and author
    if (coverRendered) {
      constexpr int boxPadding = 8;
      // Calculate the max text width for the box
      int maxTextWidth = 0;
      for (const auto& line : lines) {
        const int lineWidth = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
        if (lineWidth > maxTextWidth) {
          maxTextWidth = lineWidth;
        }
      }
      if (!truncatedAuthor.empty()) {
        const int authorWidth = renderer.getTextWidth(UI_10_FONT_ID, truncatedAuthor.c_str());
        if (authorWidth > maxTextWidth) {
          maxTextWidth = authorWidth;
        }
      }

      const int boxWidth = maxTextWidth + boxPadding * 2;
      const int boxHeight = totalTextHeight + boxPadding * 2;
      const int boxX = rect.x + (rect.width - boxWidth) / 2;
      const int boxY = titleYStart - boxPadding;

      // Draw box (inverted when selected: black box instead of white)
      renderer.fillRect(boxX, boxY, boxWidth, boxHeight, bookSelected);
      // Draw border around the box (inverted when selected: white border instead of black)
      renderer.drawRect(boxX, boxY, boxWidth, boxHeight, !bookSelected);
    }

    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, titleYStart, line.c_str(), !bookSelected);
      titleYStart += renderer.getLineHeight(UI_12_FONT_ID);
    }

    if (!truncatedAuthor.empty()) {
      titleYStart += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, titleYStart, truncatedAuthor.c_str(), !bookSelected);
    }

    // "Continue Reading" label at the bottom
    const int continueY = bookY + bookHeight - renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    if (coverRendered) {
      // Draw box behind "Continue Reading" text (inverted when selected: black box instead of white)
      const char* continueText = tr(STR_CONTINUE_READING);
      const int continueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, continueText);
      constexpr int continuePadding = 6;
      const int continueBoxWidth = continueTextWidth + continuePadding * 2;
      const int continueBoxHeight = renderer.getLineHeight(UI_10_FONT_ID) + continuePadding;
      const int continueBoxX = rect.x + (rect.width - continueBoxWidth) / 2;
      const int continueBoxY = continueY - continuePadding / 2;
      renderer.fillRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, bookSelected);
      renderer.drawRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, !bookSelected);
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, continueText, !bookSelected);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, tr(STR_CONTINUE_READING), !bookSelected);
    }
  } else {
    // No book to continue reading
    const int y =
        bookY + (bookHeight - renderer.getLineHeight(UI_12_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, "No open book");
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID), "Start reading below");
  }
}
