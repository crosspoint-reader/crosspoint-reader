#include "ButtonActionsOverviewActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <array>

#include "MappedInputManager.h"
#include "SettingInfo.h"
#include "SettingsList.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

struct ButtonRow {
  StrId submenu;     // e.g. STR_BTN_BACK — identifies which logical button
  StrId labelStrId;  // e.g. STR_BTN_BACK — display label for the row
};

// Order matches the on-screen rendering order — power → confirm.
constexpr std::array<ButtonRow, 7> kButtonRows = {{
    {StrId::STR_BTN_POWER, StrId::STR_BTN_POWER},
    {StrId::STR_BTN_BACK, StrId::STR_BTN_BACK},
    {StrId::STR_BTN_LEFT, StrId::STR_BTN_LEFT},
    {StrId::STR_BTN_RIGHT, StrId::STR_BTN_RIGHT},
    {StrId::STR_BTN_UP, StrId::STR_BTN_UP},
    {StrId::STR_BTN_DOWN, StrId::STR_BTN_DOWN},
    {StrId::STR_BTN_CONFIRM, StrId::STR_BTN_CONFIRM},
}};

// Find the SettingInfo for a given (button submenu, press kind) pair in the shared settings list.
std::string cellValue(const std::vector<SettingInfo>& settings, StrId submenu, StrId pressKind) {
  auto it = std::find_if(settings.begin(), settings.end(), [submenu, pressKind](const SettingInfo& s) {
    return s.submenu == submenu && s.nameId == pressKind && s.category == StrId::STR_CAT_CONTROLS;
  });
  if (it == settings.end()) return {};
  return it->getDisplayValue();
}

}  // namespace

void ButtonActionsOverviewActivity::onEnter() {
  Activity::onEnter();

  // Force landscape so the four-column table has room for full action labels.
  {
    RenderLock lock(*this);
    renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
  }

  requestUpdate();
}

void ButtonActionsOverviewActivity::onExit() {
  // Restore portrait — matches the rest of the settings UI.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  Activity::onExit();
}

void ButtonActionsOverviewActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    finish();
    return;
  }
}

void ButtonActionsOverviewActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, /*hasBottomHints=*/true, /*hasSideHints=*/false);

  GUI.drawHeader(renderer,
                 Rect{contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_BTN_ACTIONS_OVERVIEW), CROSSPOINT_VERSION);

  const int fontId = UI_10_FONT_ID;
  const int lineH = renderer.getLineHeight(fontId);
  const int rowStep = lineH + 4;
  const int padX = metrics.verticalSpacing * 2;

  // 4-column layout: button label, short, double, long.
  // Button column gets a fixed share, the three action columns share the remainder evenly.
  const int innerLeft = contentRect.x + padX;
  const int innerWidth = contentRect.width - padX * 2;
  const int colButtonWidth = innerWidth * 28 / 100;
  const int colActionWidth = (innerWidth - colButtonWidth) / 3;

  const int colX[4] = {
      innerLeft,
      innerLeft + colButtonWidth,
      innerLeft + colButtonWidth + colActionWidth,
      innerLeft + colButtonWidth + colActionWidth * 2,
  };
  const int colW[4] = {
      colButtonWidth - 2,
      colActionWidth - 2,
      colActionWidth - 2,
      innerLeft + innerWidth - colX[3] - 2,
  };

  int y = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // Header row (bold)
  const char* headers[4] = {
      tr(STR_BTN_OVERVIEW_HEADER_BUTTON),
      tr(STR_BTN_OVERVIEW_HEADER_SHORT),
      tr(STR_BTN_OVERVIEW_HEADER_DOUBLE),
      tr(STR_BTN_OVERVIEW_HEADER_LONG),
  };
  for (int c = 0; c < 4; c++) {
    const std::string clipped = renderer.truncatedText(fontId, headers[c], colW[c], EpdFontFamily::BOLD);
    renderer.drawText(fontId, colX[c], y, clipped.c_str(), true, EpdFontFamily::BOLD);
  }
  y += lineH + 2;

  // Underline below header
  const int underlineX[4] = {innerLeft, innerLeft + innerWidth, innerLeft + innerWidth, innerLeft};
  const int underlineY[4] = {y, y, y + 1, y + 1};
  renderer.fillPolygon(underlineX, underlineY, 4, true);
  y += 4;

  const auto settings = getSettingsList();

  // Data rows
  for (const auto& row : kButtonRows) {
    const std::string label = I18N.get(row.labelStrId);
    const std::string clippedLabel = renderer.truncatedText(fontId, label.c_str(), colW[0], EpdFontFamily::BOLD);
    renderer.drawText(fontId, colX[0], y, clippedLabel.c_str(), true, EpdFontFamily::BOLD);

    const std::string vShort = cellValue(settings, row.submenu, StrId::STR_BTN_SHORT_PRESS);
    const std::string vDouble = cellValue(settings, row.submenu, StrId::STR_BTN_DOUBLE_PRESS);
    const std::string vLong = cellValue(settings, row.submenu, StrId::STR_BTN_LONG_PRESS);

    renderer.drawText(fontId, colX[1], y, renderer.truncatedText(fontId, vShort.c_str(), colW[1]).c_str());
    renderer.drawText(fontId, colX[2], y, renderer.truncatedText(fontId, vDouble.c_str(), colW[2]).c_str());
    renderer.drawText(fontId, colX[3], y, renderer.truncatedText(fontId, vLong.c_str(), colW[3]).c_str());

    y += rowStep;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
