#include "HomeMoreActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "GlobalBookmarkIndex.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"

HomeMoreActivity::HomeMoreActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : MenuListActivity("HomeMore", renderer, mappedInput) {
  const HomeMenuAvailability availability{.hasBookmarks = !GLOBAL_BOOKMARKS.isEmpty()};
  std::vector<HomeMenuEntry> entries;
  entries.reserve(5);
  collectHomeMenuEntries(HomeMenuPlacement::More, availability, entries);

  menuItems.reserve(entries.size());
  actions.reserve(entries.size());
  for (const HomeMenuEntry& entry : entries) {
    menuItems.push_back(SettingInfo::Action(entry.label, SettingAction::None));
    actions.push_back(entry.action);
  }
}

void HomeMoreActivity::onActionSelected(const int index) {
  if (index < 0 || index >= static_cast<int>(actions.size())) return;
  activityManager.goToHomeMenuAction(actions[index]);
}

void HomeMoreActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = listContentRect();

  GUI.drawHeader(renderer, listHeaderRect(), tr(STR_MORE));

  const int contentTop = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.y + contentRect.height - contentTop - metrics.verticalSpacing * 2;
  drawMenuList(Rect{contentRect.x, contentTop, contentRect.width, contentHeight});

  drawListHints();

  renderer.displayBuffer();
}
