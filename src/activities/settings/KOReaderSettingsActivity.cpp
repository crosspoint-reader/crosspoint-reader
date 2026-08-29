#include "KOReaderSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <memory>
#include <string>

#include "KOReaderAuthActivity.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
const StrId menuNames[KOReaderSettingsActivity::MENU_ITEMS] = {
    StrId::STR_USERNAME,      StrId::STR_PASSWORD,      StrId::STR_SYNC_SERVER_URL, StrId::STR_DOCUMENT_MATCHING,
    StrId::STR_SEND_METADATA, StrId::STR_SYNC_BEHAVIOR, StrId::STR_SIGN_UP,         StrId::STR_AUTHENTICATE};
}  // namespace

KOReaderSettingsActivity::KOReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("KOReaderSettings", renderer, mappedInput) {
  // Labels never change (unlike the values, which track live KOREADER_STORE
  // state), so they're set once here rather than every buildScreen() call.
  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].label = I18N.get(menuNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

int KOReaderSettingsActivity::listCount() const { return MENU_ITEMS; }

const char* KOReaderSettingsActivity::headerTitle() const { return tr(STR_KOREADER_SYNC); }

void KOReaderSettingsActivity::activateIndex(const int index) {
  // Activation opens a keyboard/sub-activity or repaints a new value; a
  // lingering flash would gray an unrelated row.
  app.clearTapFlash();
  if (index == 0) {
    // Username
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOREADER_USERNAME),
                                                                   KOREADER_STORE.getUsername(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               KOREADER_STORE.setCredentials(kb.text, KOREADER_STORE.getPassword());
                               KOREADER_STORE.saveToFile();
                             }
                           });
  } else if (index == 1) {
    // Password
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOREADER_PASSWORD),
                                                                   KOREADER_STORE.getPassword(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), kb.text);
                               KOREADER_STORE.saveToFile();
                             }
                           });
  } else if (index == 2) {
    // Sync Server URL - prefill with https:// if empty to save typing
    const std::string currentUrl = KOREADER_STORE.getServerUrl();
    const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SYNC_SERVER_URL),
                                                                   prefillUrl, 128, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string urlToSave =
                                   (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                               KOREADER_STORE.setServerUrl(urlToSave);
                               KOREADER_STORE.saveToFile();
                             }
                           });
  } else if (index == 3) {
    // Document Matching - toggle between Filename and Binary
    const auto current = KOREADER_STORE.getMatchMethod();
    const auto newMethod =
        (current == DocumentMatchMethod::FILENAME) ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
    KOREADER_STORE.setMatchMethod(newMethod);
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (index == 4) {
    // Send Metadata - toggle on/off
    KOREADER_STORE.setSendMetadata(!KOREADER_STORE.getSendMetadata());
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (index == 5) {
    // Sync behavior - toggle between Ask and Smart
    const auto current = KOREADER_STORE.getSyncBehavior();
    const auto newBehavior = (current == KOReaderSyncBehavior::ASK_EVERY_TIME) ? KOReaderSyncBehavior::SMART
                                                                               : KOReaderSyncBehavior::ASK_EVERY_TIME;
    KOREADER_STORE.setSyncBehavior(newBehavior);
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (index == 6) {
    // Sign Up - create a new account on the sync server with the entered credentials
    if (!KOREADER_STORE.hasCredentials()) {
      return;
    }
    startActivityForResult(
        std::make_unique<KOReaderAuthActivity>(renderer, mappedInput, KOReaderAuthActivity::Mode::SIGN_UP),
        [](const ActivityResult&) {});
  } else if (index == 7) {
    // Authenticate
    if (!KOREADER_STORE.hasCredentials()) {
      // Can't authenticate without credentials - just show message briefly
      return;
    }
    startActivityForResult(std::make_unique<KOReaderAuthActivity>(renderer, mappedInput), [](const ActivityResult&) {});
  }
}

void KOReaderSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems_'s labels/actionValue were set once in the constructor; only the
  // live value text needs refreshing here, by assigning into the existing
  // rowValues_ strings (no array growth) rather than building a new
  // items/values vector on every render.
  for (int i = 0; i < MENU_ITEMS; i++) {
    if (i == 0) {
      const auto username = KOREADER_STORE.getUsername();
      rowValues_[i] = username.empty() ? tr(STR_NOT_SET) : username;
    } else if (i == 1) {
      rowValues_[i] = KOREADER_STORE.getPassword().empty() ? tr(STR_NOT_SET) : "******";
    } else if (i == 2) {
      rowValues_[i] = KOREADER_STORE.getServerUrl();
      if (rowValues_[i].empty()) {
        // Show which server the default actually is, scheme stripped for space
        std::string defaultUrl = KOREADER_STORE.getBaseUrl();
        const auto schemeEnd = defaultUrl.find("://");
        if (schemeEnd != std::string::npos) {
          defaultUrl.erase(0, schemeEnd + 3);
        }
        rowValues_[i] = std::string(tr(STR_DEFAULT_VALUE)) + ": " + defaultUrl;
      }
    } else if (i == 3) {
      rowValues_[i] =
          KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME ? tr(STR_FILENAME) : tr(STR_BINARY);
    } else if (i == 4) {
      rowValues_[i] = KOREADER_STORE.getSendMetadata() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    } else if (i == 5) {
      rowValues_[i] =
          KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART ? tr(STR_SMART_SYNC) : tr(STR_ASK_EVERY_TIME);
    } else {
      rowValues_[i] = KOREADER_STORE.hasCredentials() ? "" : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
    }
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(MENU_ITEMS);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Label at the value's font size: both sides of the row read as one unit.
  // maxLines=2 also marks the style caller-owned (see textStyleUnset).
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
