#include "RssSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int BASE_ITEMS = 4;
}

int RssSettingsActivity::getMenuItemCount() const { return isNewFeed ? BASE_ITEMS : BASE_ITEMS + 1; }

void RssSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  isNewFeed = (feedIndex < 0);
  showSaveError = false;

  if (!isNewFeed) {
    const auto* feed = RSS_STORE.getFeed(static_cast<size_t>(feedIndex));
    if (feed) {
      editFeed = *feed;
    } else {
      isNewFeed = true;
      feedIndex = -1;
    }
  }

  requestUpdate();
}

void RssSettingsActivity::onExit() { Activity::onExit(); }

void RssSettingsActivity::loop() {
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
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, menuItems);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, menuItems] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, menuItems);
    requestUpdate();
  });
}

bool RssSettingsActivity::saveFeed() {
  bool success = false;
  if (isNewFeed) {
    success = RSS_STORE.addFeed(editFeed);
    if (success) {
      isNewFeed = false;
      feedIndex = static_cast<int>(RSS_STORE.getCount()) - 1;
    } else {
      LOG_ERR("RSS", "Failed to add RSS feed");
    }
  } else {
    success = RSS_STORE.updateFeed(static_cast<size_t>(feedIndex), editFeed);
    if (!success) LOG_ERR("RSS", "Failed to update RSS feed at index %d", feedIndex);
  }

  showSaveError = !success;
  if (showSaveError) requestUpdate();
  return success;
}

void RssSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        editFeed.name = std::get<KeyboardResult>(result.data).text;
        saveFeed();
        requestUpdate();
      }
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_FEED_NAME), editFeed.name, 63,
                                                InputType::Text),
        handler);
  } else if (selectedIndex == 1) {
    const std::string prefillUrl = editFeed.url.empty() ? "https://" : editFeed.url;
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& text = std::get<KeyboardResult>(result.data).text;
        editFeed.url = (text == "https://" || text == "http://") ? "" : text;
        saveFeed();
        requestUpdate();
      }
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_RSS_FEED_URL), prefillUrl, 127,
                                                InputType::Url),
        handler);
  } else if (selectedIndex == 2) {
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        editFeed.username = std::get<KeyboardResult>(result.data).text;
        saveFeed();
        requestUpdate();
      }
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_USERNAME), editFeed.username, 63,
                                                InputType::Text),
        handler);
  } else if (selectedIndex == 3) {
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        editFeed.password = std::get<KeyboardResult>(result.data).text;
        saveFeed();
        requestUpdate();
      }
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_PASSWORD), editFeed.password, 63,
                                                InputType::Password),
        handler);
  } else if (selectedIndex == 4 && !isNewFeed) {
    if (!RSS_STORE.removeFeed(static_cast<size_t>(feedIndex))) {
      LOG_ERR("RSS", "Failed to remove RSS feed at index %d", feedIndex);
      showSaveError = true;
      requestUpdate();
      return;
    }
    finish();
  }
}

void RssSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const char* header = isNewFeed ? tr(STR_ADD_FEED) : tr(STR_RSS_READER);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, header);
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    tr(STR_RSS_URL_HINT));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + metrics.tabBarHeight;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int menuItems = getMenuItemCount();
  const StrId fieldNames[] = {StrId::STR_FEED_NAME, StrId::STR_RSS_FEED_URL, StrId::STR_USERNAME, StrId::STR_PASSWORD};

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, menuItems, selectedIndex,
      [this, &fieldNames](int index) {
        if (index < BASE_ITEMS) return std::string(I18N.get(fieldNames[index]));
        return std::string(tr(STR_DELETE_FEED));
      },
      nullptr, nullptr,
      [this](int index) {
        if (index == 0) return editFeed.name.empty() ? std::string(tr(STR_NOT_SET)) : editFeed.name;
        if (index == 1) return editFeed.url.empty() ? std::string(tr(STR_NOT_SET)) : editFeed.url;
        if (index == 2) return editFeed.username.empty() ? std::string(tr(STR_NOT_SET)) : editFeed.username;
        if (index == 3) return editFeed.password.empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
        return std::string("");
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (showSaveError) GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
  renderer.displayBuffer();
}
