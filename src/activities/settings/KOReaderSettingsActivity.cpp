#include "KOReaderSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "KOReaderAuthActivity.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

KOReaderSettingsActivity::KOReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : MenuListActivity("KOReaderSettings", renderer, mappedInput) {
  buildMenuItems();
}

void KOReaderSettingsActivity::buildMenuItems() {
  menuItems.reserve(6);
  // Username, Password, Server URL: ACTION items with custom value display
  menuItems.push_back(SettingInfo::Action(StrId::STR_SYNC_SERVER_URL, SettingAction::None)
                          .withSubcategory(StrId::STR_MENU_KOSYNC_SERVER));
  menuItems.push_back(SettingInfo::Action(StrId::STR_USERNAME, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_PASSWORD, SettingAction::None));

  // Document matching: DynamicEnum toggling between Filename and Binary
  menuItems.push_back(SettingInfo::DynamicEnum(
      StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
      [](void*) { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
      [](void*, uint8_t v) {
        KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
        KOREADER_STORE.saveToFile();
      }));

  // Authenticate and Register: ACTION items
  menuItems.push_back(
      SettingInfo::Action(StrId::STR_AUTHENTICATE, SettingAction::None).withSubcategory(StrId::STR_MENU_KOSYNC_AUTH));
  menuItems.push_back(SettingInfo::Action(StrId::STR_REGISTER, SettingAction::None));
}

std::string KOReaderSettingsActivity::getItemValueString(int index) const {
  const auto& item = menuItems[index];

  if (item.nameId == StrId::STR_USERNAME) {
    auto username = KOREADER_STORE.getUsername();
    return username.empty() ? std::string(tr(STR_NOT_SET)) : username;
  }
  if (item.nameId == StrId::STR_PASSWORD) {
    return KOREADER_STORE.getPassword().empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
  }
  if (item.nameId == StrId::STR_SYNC_SERVER_URL) {
    auto serverUrl = KOREADER_STORE.getServerUrl();
    return serverUrl.empty() ? std::string(tr(STR_DEFAULT_VALUE)) : serverUrl;
  }
  if (item.nameId == StrId::STR_AUTHENTICATE || item.nameId == StrId::STR_REGISTER) {
    return KOREADER_STORE.hasCredentials() ? "" : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
  }

  return MenuListActivity::getItemValueString(index);
}

void KOReaderSettingsActivity::onActionSelected(int index) {
  const auto& item = menuItems[index];

  if (item.nameId == StrId::STR_USERNAME) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOREADER_USERNAME),
                                                                   KOREADER_STORE.getUsername(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               KOREADER_STORE.setCredentials(kb.text, KOREADER_STORE.getPassword());
                               KOREADER_STORE.saveToFile();
                             }
                           });
  } else if (item.nameId == StrId::STR_PASSWORD) {
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
  } else if (item.nameId == StrId::STR_SYNC_SERVER_URL) {
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
  } else if (item.nameId == StrId::STR_AUTHENTICATE) {
    if (!KOREADER_STORE.hasCredentials()) return;
    startActivityForResult(
        std::make_unique<KOReaderAuthActivity>(renderer, mappedInput, KOReaderAuthActivity::Mode::LOGIN),
        [](const ActivityResult&) {});
  } else if (item.nameId == StrId::STR_REGISTER) {
    if (!KOREADER_STORE.hasCredentials()) return;
    startActivityForResult(
        std::make_unique<KOReaderAuthActivity>(renderer, mappedInput, KOReaderAuthActivity::Mode::REGISTER),
        [](const ActivityResult&) {});
  }
}

void KOReaderSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_KOREADER_SYNC));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing * 2;
  drawMenuList(Rect{contentRect.x, contentTop, contentRect.width, contentHeight});

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
