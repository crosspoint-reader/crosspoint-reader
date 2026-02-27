#include "OpdsSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Editable fields: Name, URL, Username, Password.
// Existing servers also show a Delete option (BASE_ITEMS + 1).
constexpr int BASE_ITEMS = 4;
}  // namespace

int OpdsSettingsActivity::getMenuItemCount() const {
  return isNewServer ? BASE_ITEMS : BASE_ITEMS + 1;  // +1 for Delete
}

void OpdsSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  isNewServer = (serverIndex < 0);

  if (!isNewServer) {
    const auto* server = OPDS_STORE.getServer(static_cast<size_t>(serverIndex));
    if (server) {
      editServer = *server;
    } else {
      // Server was deleted between navigation and entering this screen — treat as new
      isNewServer = true;
      serverIndex = -1;
    }
  }

  requestUpdate();
}

void OpdsSettingsActivity::onExit() { Activity::onExit(); }

void OpdsSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int menuItems = getMenuItemCount();
  buttonNavigator.onNext([this, menuItems] {
    selectedIndex = (selectedIndex + 1) % menuItems;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuItems] {
    selectedIndex = (selectedIndex + menuItems - 1) % menuItems;
    requestUpdate();
  });
}

void OpdsSettingsActivity::saveServer() {
  if (isNewServer) {
    OPDS_STORE.addServer(editServer);
    // After the first field is saved, promote to an existing server so
    // subsequent field edits update in-place rather than creating duplicates.
    isNewServer = false;
    serverIndex = static_cast<int>(OPDS_STORE.getCount()) - 1;
  } else {
    OPDS_STORE.updateServer(static_cast<size_t>(serverIndex), editServer);
  }
}

void OpdsSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // Server Name
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.name = kb.text;
        saveServer();
      }
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SERVER_NAME), editServer.name, 63, false),
        handler);
  } else if (selectedIndex == 1) {
    // Server URL
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.url = kb.text;
        saveServer();
      }
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_OPDS_SERVER_URL), editServer.url, 127,
                                                false),
        handler);
  } else if (selectedIndex == 2) {
    // Username
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.username = kb.text;
        saveServer();
      }
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_USERNAME), editServer.username, 63,
                                                false),
        handler);
  } else if (selectedIndex == 3) {
    // Password
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.password = kb.text;
        saveServer();
      }
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_PASSWORD), editServer.password, 63,
                                                false),
        handler);
  } else if (selectedIndex == 4 && !isNewServer) {
    // Delete server
    OPDS_STORE.removeServer(static_cast<size_t>(serverIndex));
    finish();
  }
}

void OpdsSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const char* header = isNewServer ? tr(STR_ADD_SERVER) : tr(STR_OPDS_BROWSER);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, header);
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    tr(STR_CALIBRE_URL_HINT));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + metrics.tabBarHeight;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int menuItems = getMenuItemCount();

  const StrId fieldNames[] = {StrId::STR_SERVER_NAME, StrId::STR_OPDS_SERVER_URL, StrId::STR_USERNAME,
                              StrId::STR_PASSWORD};

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, menuItems, static_cast<int>(selectedIndex),
      [this, &fieldNames](int index) {
        if (index < BASE_ITEMS) {
          return std::string(I18N.get(fieldNames[index]));
        }
        return std::string(tr(STR_DELETE_SERVER));
      },
      nullptr, nullptr,
      [this](int index) {
        if (index == 0) {
          return editServer.name.empty() ? std::string(tr(STR_NOT_SET)) : editServer.name;
        } else if (index == 1) {
          return editServer.url.empty() ? std::string(tr(STR_NOT_SET)) : editServer.url;
        } else if (index == 2) {
          return editServer.username.empty() ? std::string(tr(STR_NOT_SET)) : editServer.username;
        } else if (index == 3) {
          return editServer.password.empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
        }
        return std::string("");
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
