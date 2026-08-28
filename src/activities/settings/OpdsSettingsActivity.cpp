#include "OpdsSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
// Editable fields: Name, URL, Username, Password, Custom Header 1, Custom
// Header 2. Existing servers also show a Delete option (BASE_ITEMS + 1).
constexpr int BASE_ITEMS = 6;
constexpr int CUSTOM_HEADER_1_INDEX = 4;
constexpr int CUSTOM_HEADER_2_INDEX = 5;

std::string trim(const std::string& s) {
  const size_t start = s.find_first_not_of(" \t");
  if (start == std::string::npos) return "";
  const size_t end = s.find_last_not_of(" \t");
  return s.substr(start, end - start + 1);
}

// "Name: Value" -> trimmed name/value. A missing colon treats the whole
// entry as a bare header name with no value.
void parseCustomHeader(const std::string& line, HttpHeader& header) {
  const size_t colon = line.find(':');
  if (colon == std::string::npos) {
    header.name = trim(line);
    header.value.clear();
  } else {
    header.name = trim(line.substr(0, colon));
    header.value = trim(line.substr(colon + 1));
  }
}

std::string formatCustomHeader(const HttpHeader& header) {
  if (header.name.empty()) return "";
  return header.value.empty() ? header.name : header.name + ": " + header.value;
}

std::string formatCustomHeaderMasked(const HttpHeader& header) {
  if (header.name.empty()) return "";
  return header.value.empty() ? header.name : header.name + ": ******";
}
}  // namespace

OpdsSettingsActivity::OpdsSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const int serverIndex)
    : UiListActivity("OpdsSettings", renderer, mappedInput), serverIndex(serverIndex) {
  // Labels never change (unlike the values, which track editServer's fields
  // live), so they're set once here rather than every buildScreen() call.
  static constexpr StrId fieldNames[BASE_ITEMS] = {StrId::STR_SERVER_NAME,     StrId::STR_OPDS_SERVER_URL,
                                                   StrId::STR_USERNAME,        StrId::STR_PASSWORD,
                                                   StrId::STR_CUSTOM_HEADER_1, StrId::STR_CUSTOM_HEADER_2};
  for (int i = 0; i < BASE_ITEMS; i++) {
    fieldRowItems[i].label = I18N.get(fieldNames[i]);
    fieldRowItems[i].actionValue = static_cast<int16_t>(i);
  }
  fieldRowItems[BASE_ITEMS].label = tr(STR_DELETE_SERVER);
  fieldRowItems[BASE_ITEMS].actionValue = static_cast<int16_t>(BASE_ITEMS);
}

int OpdsSettingsActivity::getMenuItemCount() const {
  return isNewServer ? BASE_ITEMS : BASE_ITEMS + 1;  // +1 for Delete
}

void OpdsSettingsActivity::onEnter() {
  UiListActivity::onEnter();

  isNewServer = (serverIndex < 0);
  showSaveError = false;

  if (!isNewServer) {
    // Edit flow: copy the selected server into local editable state.
    // Changes are persisted field-by-field through saveServer().
    const auto* server = OPDS_STORE.getServer(static_cast<size_t>(serverIndex));
    if (server) {
      editServer = *server;
    } else {
      // Server was deleted between navigation and entering this screen — treat as new
      isNewServer = true;
      serverIndex = -1;
    }
  }
}

void OpdsSettingsActivity::activateIndex(const int index) {
  nav.selected = index;
  // Activation opens a keyboard or leaves the screen; a lingering flash would
  // gray an unrelated row.
  app.clearTapFlash();
  handleSelection();
}

bool OpdsSettingsActivity::saveServer() {
  bool success = false;

  if (isNewServer) {
    // Create flow: first save inserts a new server record into the multi-server store.
    success = OPDS_STORE.addServer(editServer);
    if (success) {
      // After the first successful save, promote to an existing server so
      // subsequent field edits update in-place rather than creating duplicates.
      isNewServer = false;
      serverIndex = static_cast<int>(OPDS_STORE.getCount()) - 1;
    } else {
      LOG_ERR("OPS", "Failed to add OPDS server");
    }
  } else {
    // Edit flow: update the same server entry in-place.
    success = OPDS_STORE.updateServer(static_cast<size_t>(serverIndex), editServer);
    if (!success) {
      LOG_ERR("OPS", "Failed to update OPDS server at index %d", serverIndex);
    }
  }

  showSaveError = !success;
  if (showSaveError) {
    requestUpdate();
  }

  return success;
}

void OpdsSettingsActivity::handleSelection() {
  // Each field edit is saved immediately so partially configured servers
  // survive navigation and power-loss scenarios.
  if (nav.selected == 0) {
    // Server Name
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.name = kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SERVER_NAME),
                                                                   editServer.name, 63, InputType::Text),
                           handler);
  } else if (nav.selected == 1) {
    // Server URL
    const std::string prefillUrl = editServer.url.empty() ? "https://" : editServer.url;
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.url = (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_OPDS_SERVER_URL),
                                                                   prefillUrl, 127, InputType::Url),
                           handler);
  } else if (nav.selected == 2) {
    // Username
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.username = kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_USERNAME),
                                                                   editServer.username, 63, InputType::Text),
                           handler);
  } else if (nav.selected == 3) {
    // Password
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.password = kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_PASSWORD),
                                                                   editServer.password, 63, InputType::Text),
                           handler);
  } else if (nav.selected == CUSTOM_HEADER_1_INDEX || nav.selected == CUSTOM_HEADER_2_INDEX) {
    // Custom Header 1 or 2 - single "Name: Value" line, parsed on save.
    const size_t slot = static_cast<size_t>(nav.selected == CUSTOM_HEADER_1_INDEX ? 0 : 1);
    const StrId label = nav.selected == CUSTOM_HEADER_1_INDEX ? StrId::STR_CUSTOM_HEADER_1 : StrId::STR_CUSTOM_HEADER_2;
    const std::string prefill = formatCustomHeader(editServer.customHeaders[slot]);
    auto handler = [this, slot](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        parseCustomHeader(kb.text, editServer.customHeaders[slot]);
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, I18N.get(label), prefill, 160, InputType::Text),
        handler);
  } else if (nav.selected == BASE_ITEMS && !isNewServer) {
    // Delete flow is only available for existing servers.
    if (!OPDS_STORE.removeServer(static_cast<size_t>(serverIndex))) {
      LOG_ERR("OPS", "Failed to remove OPDS server at index %d", serverIndex);
      showSaveError = true;
      requestUpdate();
      return;
    }
    finish();
  }
}

void OpdsSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints; derived
  // from the safe area so board bezel insets apply (same as LanguageSelect).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x)});

  // URL hint where the old sub-header band sat.
  const fui::Rect band = screen.takeTop(static_cast<int16_t>(metrics.tabBarHeight));
  const int16_t pad = screen.theme().headerSidePadding;
  screen.target().text(band.inset(fui::Insets{0, pad, 0, pad}), tr(STR_CALIBRE_URL_HINT), screen.theme().smallText);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // fieldRowItems' labels/actionValue were set once in the constructor; only
  // the live value pointers (already pointing at editServer's own fields, no
  // new strings built) need refreshing here.
  fieldRowItems[0].value = editServer.name.empty() ? tr(STR_NOT_SET) : editServer.name.c_str();
  fieldRowItems[1].value = editServer.url.empty() ? tr(STR_NOT_SET) : editServer.url.c_str();
  fieldRowItems[2].value = editServer.username.empty() ? tr(STR_NOT_SET) : editServer.username.c_str();
  fieldRowItems[3].value = editServer.password.empty() ? tr(STR_NOT_SET) : "******";
  for (size_t i = 0; i < OpdsServer::MAX_CUSTOM_HEADERS; i++) {
    headerDisplayBuf[i] = formatCustomHeaderMasked(editServer.customHeaders[i]);
    fieldRowItems[CUSTOM_HEADER_1_INDEX + i].value =
        headerDisplayBuf[i].empty() ? tr(STR_NOT_SET) : headerDisplayBuf[i].c_str();
  }

  fui::ListProps props;
  props.items = fieldRowItems;
  props.count = static_cast<uint16_t>(getMenuItemCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  syncListViewport(screen, props);
  screen.list(props);
}

const char* OpdsSettingsActivity::headerTitle() const {
  // Reuse STR_OPDS_BROWSER as the "edit existing server" title.
  // New server creation uses STR_ADD_SERVER.
  return isNewServer ? tr(STR_ADD_SERVER) : tr(STR_OPDS_BROWSER);
}

void OpdsSettingsActivity::drawFooter() {
  UiListActivity::drawFooter();
  if (showSaveError) {
    GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
  }
}
