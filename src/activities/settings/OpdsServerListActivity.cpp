#include "OpdsServerListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CalibreSettingsActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

int OpdsServerListActivity::getItemCount() const {
  int count = static_cast<int>(OPDS_STORE.getCount());
  // In settings mode, append a virtual "Add Server" item; in picker mode, only show real servers
  if (!isPickerMode()) {
    count++;
  }
  return count;
}

void OpdsServerListActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  // Reload from disk in case servers were added/removed by a subactivity or the web UI
  OPDS_STORE.loadFromFile();
  selectedIndex = 0;
  requestUpdate();
}

void OpdsServerListActivity::onExit() { ActivityWithSubactivity::onExit(); }

void OpdsServerListActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int itemCount = getItemCount();
  if (itemCount > 0) {
    buttonNavigator.onNext([this, itemCount] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, itemCount] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
      requestUpdate();
    });
  }
}

void OpdsServerListActivity::handleSelection() {
  const auto serverCount = static_cast<int>(OPDS_STORE.getCount());

  if (isPickerMode()) {
    // Picker mode: selecting a server triggers the callback instead of opening the editor
    if (selectedIndex < serverCount) {
      onServerSelected(static_cast<size_t>(selectedIndex));
    }
    return;
  }

  // Settings mode: open editor for selected server, or create a new one
  auto onEditDone = [this] {
    exitActivity();
    selectedIndex = 0;
    requestUpdate();
  };

  if (selectedIndex < serverCount) {
    exitActivity();
    enterNewActivity(new CalibreSettingsActivity(renderer, mappedInput, onEditDone, selectedIndex));
  } else {
    exitActivity();
    enterNewActivity(new CalibreSettingsActivity(renderer, mappedInput, onEditDone, -1));
  }
}

void OpdsServerListActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_OPDS_SERVERS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int itemCount = getItemCount();

  if (itemCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_SERVERS));
  } else {
    const auto& servers = OPDS_STORE.getServers();
    const auto serverCount = static_cast<int>(servers.size());

    // Primary label: server name (falling back to URL if unnamed).
    // Secondary label: server URL (shown as subtitle when name is set).
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex,
        [&servers, serverCount](int index) {
          if (index < serverCount) {
            const auto& server = servers[index];
            return server.name.empty() ? server.url : server.name;
          }
          return std::string(I18n::getInstance().get(StrId::STR_ADD_SERVER));
        },
        [&servers, serverCount](int index) {
          if (index < serverCount && !servers[index].name.empty()) {
            return servers[index].url;
          }
          return std::string("");
        });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
