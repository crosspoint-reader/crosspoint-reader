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
constexpr int IDX_USERNAME = 0;
constexpr int IDX_PASSWORD = 1;
constexpr int IDX_SERVER_URL = 2;
constexpr int IDX_CUSTOM_HEADER_1 = 3;
constexpr int IDX_CUSTOM_HEADER_2 = 4;
constexpr int IDX_MATCH_METHOD = 5;
constexpr int IDX_SEND_METADATA = 6;
constexpr int IDX_SYNC_BEHAVIOR = 7;
constexpr int IDX_SIGN_UP = 8;
constexpr int IDX_AUTHENTICATE = 9;

const StrId menuNames[KOReaderSettingsActivity::MENU_ITEMS] = {
    StrId::STR_USERNAME,        StrId::STR_PASSWORD,          StrId::STR_SYNC_SERVER_URL, StrId::STR_CUSTOM_HEADER_1,
    StrId::STR_CUSTOM_HEADER_2, StrId::STR_DOCUMENT_MATCHING, StrId::STR_SEND_METADATA,   StrId::STR_SYNC_BEHAVIOR,
    StrId::STR_SIGN_UP,         StrId::STR_AUTHENTICATE};

std::string trim(const std::string& s) {
  const size_t start = s.find_first_not_of(" \t");
  if (start == std::string::npos) return "";
  const size_t end = s.find_last_not_of(" \t");
  return s.substr(start, end - start + 1);
}

// "Name: Value" -> trimmed name/value. A missing colon treats the whole
// entry as a bare header name with no value.
void parseCustomHeader(const std::string& line, KOReaderCustomHeader& header) {
  const size_t colon = line.find(':');
  if (colon == std::string::npos) {
    header.name = trim(line);
    header.value.clear();
  } else {
    header.name = trim(line.substr(0, colon));
    header.value = trim(line.substr(colon + 1));
  }
}

std::string formatCustomHeader(const KOReaderCustomHeader& header) {
  if (header.name.empty()) return "";
  return header.value.empty() ? header.name : header.name + ": " + header.value;
}

std::string formatCustomHeaderMasked(const KOReaderCustomHeader& header) {
  if (header.name.empty()) return "";
  return header.value.empty() ? header.name : header.name + ": ******";
}
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
  if (index == IDX_USERNAME) {
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
  } else if (index == IDX_PASSWORD) {
    // Password
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOREADER_PASSWORD),
                                                KOREADER_STORE.getPassword(), 64, InputType::Password),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), kb.text);
            KOREADER_STORE.saveToFile();
          }
        });
  } else if (index == IDX_SERVER_URL) {
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
  } else if (index == IDX_CUSTOM_HEADER_1 || index == IDX_CUSTOM_HEADER_2) {
    // Custom Header 1 or 2 - single "Name: Value" line, parsed on save.
    const size_t slot = static_cast<size_t>(index == IDX_CUSTOM_HEADER_1 ? 0 : 1);
    const StrId label = index == IDX_CUSTOM_HEADER_1 ? StrId::STR_CUSTOM_HEADER_1 : StrId::STR_CUSTOM_HEADER_2;
    const std::string prefill = formatCustomHeader(KOREADER_STORE.getCustomHeaders()[slot]);
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, I18N.get(label), prefill, 160, InputType::Text),
        [this, slot](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            KOReaderCustomHeader header;
            parseCustomHeader(kb.text, header);
            KOREADER_STORE.setCustomHeader(slot, header.name, header.value);
            KOREADER_STORE.saveToFile();
          }
        });
  } else if (index == IDX_MATCH_METHOD) {
    // Document Matching - toggle between Filename and Binary
    const auto current = KOREADER_STORE.getMatchMethod();
    const auto newMethod =
        (current == DocumentMatchMethod::FILENAME) ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
    KOREADER_STORE.setMatchMethod(newMethod);
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (index == IDX_SEND_METADATA) {
    // Send Metadata - toggle on/off
    KOREADER_STORE.setSendMetadata(!KOREADER_STORE.getSendMetadata());
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (index == IDX_SYNC_BEHAVIOR) {
    // Sync behavior - toggle between Ask and Smart
    const auto current = KOREADER_STORE.getSyncBehavior();
    const auto newBehavior = (current == KOReaderSyncBehavior::ASK_EVERY_TIME) ? KOReaderSyncBehavior::SMART
                                                                               : KOReaderSyncBehavior::ASK_EVERY_TIME;
    KOREADER_STORE.setSyncBehavior(newBehavior);
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (index == IDX_SIGN_UP) {
    // Sign Up - create a new account on the sync server with the entered credentials
    if (!KOREADER_STORE.hasCredentials()) {
      return;
    }
    startActivityForResult(
        std::make_unique<KOReaderAuthActivity>(renderer, mappedInput, KOReaderAuthActivity::Mode::SIGN_UP),
        [](const ActivityResult&) {});
  } else if (index == IDX_AUTHENTICATE) {
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
    if (i == IDX_USERNAME) {
      const auto username = KOREADER_STORE.getUsername();
      rowValues_[i] = username.empty() ? tr(STR_NOT_SET) : username;
    } else if (i == IDX_PASSWORD) {
      rowValues_[i] = KOREADER_STORE.getPassword().empty() ? tr(STR_NOT_SET) : "******";
    } else if (i == IDX_SERVER_URL) {
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
    } else if (i == IDX_CUSTOM_HEADER_1 || i == IDX_CUSTOM_HEADER_2) {
      const size_t slot = static_cast<size_t>(i == IDX_CUSTOM_HEADER_1 ? 0 : 1);
      rowValues_[i] = formatCustomHeaderMasked(KOREADER_STORE.getCustomHeaders()[slot]);
      if (rowValues_[i].empty()) rowValues_[i] = tr(STR_NOT_SET);
    } else if (i == IDX_MATCH_METHOD) {
      rowValues_[i] =
          KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME ? tr(STR_FILENAME) : tr(STR_BINARY);
    } else if (i == IDX_SEND_METADATA) {
      rowValues_[i] = KOREADER_STORE.getSendMetadata() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    } else if (i == IDX_SYNC_BEHAVIOR) {
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
  syncListViewport(screen, props);
  screen.list(props);
}
