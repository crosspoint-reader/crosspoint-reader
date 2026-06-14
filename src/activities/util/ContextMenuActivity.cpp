#include "ContextMenuActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kSideMargin = 40;    // gap between the box and the screen edges
constexpr int kBoxPadding = 16;    // inner padding around the box content
constexpr int kRowPadding = 10;    // vertical padding inside each menu row
constexpr int kRowTextInset = 12;  // horizontal inset of row text / highlight
constexpr int kCornerRadius = 10;
constexpr int kHaloWidth = 3;  // thin white border so the box lifts off the content behind it
}  // namespace

ContextMenuActivity::ContextMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                         std::vector<Item> items)
    : Activity("ContextMenu", renderer, mappedInput), title(std::move(title)), items(std::move(items)) {}

void ContextMenuActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  // The long-press that opened us ends on a Confirm release; if it is somehow still held,
  // swallow the upcoming release so it doesn't instantly activate the first item.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate(true);
}

void ContextMenuActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
      return;
    }
    if (items.empty()) return;
    setResult(MenuResult{items[selectedIndex].action});
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }

  const int itemCount = static_cast<int>(items.size());
  buttonNavigator.onNext([this, itemCount] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, itemCount] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
    requestUpdate();
  });
}

void ContextMenuActivity::render(RenderLock&&) {
  // NOTE: intentionally no clearScreen() — we composite over the caller's last frame.
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

  const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int itemLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int rowHeight = itemLineHeight + kRowPadding * 2;
  const int titleBlock = titleLineHeight + kBoxPadding;  // title text + gap down to the separator

  const int boxW = screenW - kSideMargin * 2;
  const int boxH = kBoxPadding + titleBlock + static_cast<int>(items.size()) * rowHeight + kBoxPadding;
  const int boxX = (screenW - boxW) / 2;
  const int boxY = (screenH - boxH) / 2;

  const int innerX = boxX + kBoxPadding;
  const int innerW = boxW - kBoxPadding * 2;

  // Box: white halo, white fill, black border.
  renderer.fillRoundedRect(boxX - kHaloWidth, boxY - kHaloWidth, boxW + kHaloWidth * 2, boxH + kHaloWidth * 2,
                           kCornerRadius + kHaloWidth, Color::White);
  renderer.fillRoundedRect(boxX, boxY, boxW, boxH, kCornerRadius, Color::White);
  renderer.drawRoundedRect(boxX, boxY, boxW, boxH, 2, kCornerRadius, true);

  // Title (the entry the menu acts on), bold and truncated to the box.
  const std::string safeTitle = renderer.truncatedText(UI_10_FONT_ID, title.c_str(), innerW, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, innerX, boxY + kBoxPadding, safeTitle.c_str(), true, EpdFontFamily::BOLD);

  // Separator under the title.
  const int separatorY = boxY + kBoxPadding + titleLineHeight + kBoxPadding / 2;
  renderer.drawLine(innerX, separatorY, boxX + boxW - kBoxPadding, separatorY, 1, true);

  // Items.
  const int itemsTop = boxY + kBoxPadding + titleBlock;
  for (int i = 0; i < static_cast<int>(items.size()); ++i) {
    const int rowY = itemsTop + i * rowHeight;
    const bool selected = (i == selectedIndex);
    if (selected) {
      renderer.fillRoundedRect(innerX, rowY, innerW, rowHeight, 6, Color::Black);
    }
    const char* label = I18N.get(items[i].labelId);
    const std::string safeLabel = renderer.truncatedText(UI_12_FONT_ID, label, innerW - kRowTextInset * 2);
    renderer.drawText(UI_12_FONT_ID, innerX + kRowTextInset, rowY + kRowPadding, safeLabel.c_str(), !selected);
  }

  // Wipe the launching activity's bottom chrome (its button hints + path line) so it doesn't show
  // through next to this menu's own Back/Select hints.
  {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int chromeH = metrics.buttonHintsHeight + metrics.verticalSpacing * 2 + renderer.getLineHeight(SMALL_FONT_ID);
    renderer.fillRect(0, screenH - chromeH, screenW, chromeH, false);
  }

  // Button hints. Up/Down only matter when there is more than one item to move between.
  const bool multiItem = items.size() > 1;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), multiItem ? tr(STR_DIR_UP) : "",
                                            multiItem ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}
