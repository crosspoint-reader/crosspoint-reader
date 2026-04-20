#include "BookFusionSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "BookFusionAuthActivity.h"
#include "BookFusionBrowserActivity.h"
#include "BookFusionTokenStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int LINK_INDEX = 0;
constexpr int UNLINK_INDEX = 1;
constexpr int BROWSE_INDEX = 2;

const StrId menuNames[BookFusionSettingsActivity::MENU_ITEMS] = {StrId::STR_BF_LINK_ACCOUNT, StrId::STR_BF_UNLINK,
                                                                 StrId::STR_BF_BROWSE_LIBRARY};
}  // namespace

void BookFusionSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void BookFusionSettingsActivity::onExit() { Activity::onExit(); }

void BookFusionSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void BookFusionSettingsActivity::handleSelection() {
  if (selectedIndex == LINK_INDEX) {
    startActivityForResult(std::make_unique<BookFusionAuthActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) { requestUpdate(); });
  } else if (selectedIndex == UNLINK_INDEX) {
    BF_TOKEN_STORE.clearToken();
    requestUpdate();
  } else if (selectedIndex == BROWSE_INDEX) {
    if (BF_TOKEN_STORE.hasToken()) {
      startActivityForResult(std::make_unique<BookFusionBrowserActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
    }
  }
}

void BookFusionSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BF_SYNC));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, MENU_ITEMS, static_cast<int>(selectedIndex),
      [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr, nullptr,
      [](int index) -> std::string {
        if (index == LINK_INDEX) {
          return BF_TOKEN_STORE.hasToken() ? std::string(tr(STR_BF_LINKED)) : std::string(tr(STR_BF_NOT_LINKED));
        }
        if (index == BROWSE_INDEX && !BF_TOKEN_STORE.hasToken()) {
          return std::string(tr(STR_BF_NOT_LINKED));
        }
        return "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
