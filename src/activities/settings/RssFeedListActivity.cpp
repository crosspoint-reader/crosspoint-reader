#include "RssFeedListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "RssFeedStore.h"
#include "RssSettingsActivity.h"
#include "activities/ActivityManager.h"
#include "activities/browser/RssFeedBrowserActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

int RssFeedListActivity::getItemCount() const {
  int count = static_cast<int>(RSS_STORE.getCount());
  if (!pickerMode) count++;
  return count;
}

void RssFeedListActivity::onEnter() {
  Activity::onEnter();
  RSS_STORE.loadFromFile();
  selectedIndex = 0;
  requestUpdate();
}

void RssFeedListActivity::onExit() { Activity::onExit(); }

void RssFeedListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (pickerMode) {
      activityManager.goHome(HomeMenuItem::RSS_READER);
    } else {
      finish();
    }
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

void RssFeedListActivity::handleSelection() {
  const auto feedCount = static_cast<int>(RSS_STORE.getCount());

  if (pickerMode) {
    if (selectedIndex < feedCount) {
      const auto* feed = RSS_STORE.getFeed(static_cast<size_t>(selectedIndex));
      if (feed) activityManager.replaceActivity(std::make_unique<RssFeedBrowserActivity>(renderer, mappedInput, *feed));
    }
    return;
  }

  auto resultHandler = [this](const ActivityResult&) {
    RSS_STORE.loadFromFile();
    selectedIndex = 0;
  };

  if (selectedIndex < feedCount) {
    startActivityForResult(std::make_unique<RssSettingsActivity>(renderer, mappedInput, selectedIndex), resultHandler);
  } else {
    startActivityForResult(std::make_unique<RssSettingsActivity>(renderer, mappedInput, -1), resultHandler);
  }
}

void RssFeedListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_RSS_FEEDS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int itemCount = getItemCount();

  if (itemCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_FEEDS));
  } else {
    const auto& feeds = RSS_STORE.getFeeds();
    const auto feedCount = static_cast<int>(feeds.size());
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex,
        [&feeds, feedCount](int index) {
          if (index < feedCount) {
            const auto& feed = feeds[index];
            return feed.name.empty() ? feed.url : feed.name;
          }
          return std::string(I18N.get(StrId::STR_ADD_FEED));
        },
        [&feeds, feedCount](int index) {
          if (index < feedCount && !feeds[index].name.empty()) return feeds[index].url;
          return std::string("");
        });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
