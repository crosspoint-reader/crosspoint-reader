#include "WallabagSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WallabagCredentialStore.h>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 6;
const StrId menuNames[MENU_ITEMS] = {
    StrId::STR_WALLABAG_SERVER_URL, StrId::STR_WALLABAG_CLIENT_ID,    StrId::STR_WALLABAG_CLIENT_SECRET,
    StrId::STR_WALLABAG_USERNAME,   StrId::STR_WALLABAG_PASSWORD,      StrId::STR_WALLABAG_ARTICLE_LIMIT,
};
}  // namespace

constexpr uint8_t WallabagSettingsActivity::LIMIT_PRESETS[];

void WallabagSettingsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void WallabagSettingsActivity::onExit() { ActivityWithSubactivity::onExit(); }

void WallabagSettingsActivity::loop() {
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

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void WallabagSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // Server URL
    const std::string current = WALLABAG_STORE.getServerUrl();
    const std::string prefill = current.empty() ? "https://" : current;
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, tr(STR_WALLABAG_SERVER_URL), prefill, 128, false,
        [this](const std::string& val) {
          const std::string toSave = (val == "https://" || val == "http://") ? "" : val;
          WALLABAG_STORE.setServerUrl(toSave);
          WALLABAG_STORE.saveToFile();
          exitActivity();
          requestUpdate();
        },
        [this]() {
          exitActivity();
          requestUpdate();
        }));
  } else if (selectedIndex == 1) {
    // Client ID
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, tr(STR_WALLABAG_CLIENT_ID), WALLABAG_STORE.getClientId(), 64, false,
        [this](const std::string& val) {
          WALLABAG_STORE.setClientId(val);
          WALLABAG_STORE.saveToFile();
          exitActivity();
          requestUpdate();
        },
        [this]() {
          exitActivity();
          requestUpdate();
        }));
  } else if (selectedIndex == 2) {
    // Client Secret
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, tr(STR_WALLABAG_CLIENT_SECRET), WALLABAG_STORE.getClientSecret(), 64, false,
        [this](const std::string& val) {
          WALLABAG_STORE.setClientSecret(val);
          WALLABAG_STORE.saveToFile();
          exitActivity();
          requestUpdate();
        },
        [this]() {
          exitActivity();
          requestUpdate();
        }));
  } else if (selectedIndex == 3) {
    // Username
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, tr(STR_WALLABAG_USERNAME), WALLABAG_STORE.getUsername(), 64, false,
        [this](const std::string& val) {
          WALLABAG_STORE.setUsername(val);
          WALLABAG_STORE.saveToFile();
          exitActivity();
          requestUpdate();
        },
        [this]() {
          exitActivity();
          requestUpdate();
        }));
  } else if (selectedIndex == 4) {
    // Password
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, tr(STR_WALLABAG_PASSWORD), WALLABAG_STORE.getPassword(), 64, false,
        [this](const std::string& val) {
          WALLABAG_STORE.setPassword(val);
          WALLABAG_STORE.saveToFile();
          exitActivity();
          requestUpdate();
        },
        [this]() {
          exitActivity();
          requestUpdate();
        }));
  } else if (selectedIndex == 5) {
    // Article Limit - cycle through presets
    cycleArticleLimit();
  }
}

void WallabagSettingsActivity::cycleArticleLimit() {
  const uint8_t current = WALLABAG_STORE.getArticleLimit();
  // Find current preset index
  int nextIdx = 0;
  for (int i = 0; i < LIMIT_PRESET_COUNT; i++) {
    if (LIMIT_PRESETS[i] == current) {
      nextIdx = (i + 1) % LIMIT_PRESET_COUNT;
      break;
    }
  }
  WALLABAG_STORE.setArticleLimit(LIMIT_PRESETS[nextIdx]);
  WALLABAG_STORE.saveToFile();
  requestUpdate();
}

std::string WallabagSettingsActivity::articleLimitLabel() const {
  const uint8_t limit = WALLABAG_STORE.getArticleLimit();
  if (limit == 0) return std::string(tr(STR_WALLABAG_UNLIMITED));
  return std::to_string(limit);
}

void WallabagSettingsActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WALLABAG));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEMS),
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr,
      nullptr,
      [this](int index) -> std::string {
        if (index == 0) {
          const auto& v = WALLABAG_STORE.getServerUrl();
          return v.empty() ? std::string(tr(STR_NOT_SET)) : v;
        } else if (index == 1) {
          const auto& v = WALLABAG_STORE.getClientId();
          return v.empty() ? std::string(tr(STR_NOT_SET)) : v;
        } else if (index == 2) {
          return WALLABAG_STORE.getClientSecret().empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
        } else if (index == 3) {
          const auto& v = WALLABAG_STORE.getUsername();
          return v.empty() ? std::string(tr(STR_NOT_SET)) : v;
        } else if (index == 4) {
          return WALLABAG_STORE.getPassword().empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
        } else if (index == 5) {
          return articleLimitLabel();
        }
        return std::string(tr(STR_NOT_SET));
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
