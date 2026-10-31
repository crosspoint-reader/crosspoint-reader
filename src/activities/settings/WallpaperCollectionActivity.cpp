#include "WallpaperCollectionActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/wallpapers/WallpaperPackRegistry.h"

namespace fui = freeink::ui;

void WallpaperCollectionActivity::onEnter() {
  UiListActivity::onEnter();
  refreshChoices();
}

void WallpaperCollectionActivity::refreshChoices() {

  auto& registry = WallpaperPackRegistry::getInstance();
  registry.scan();
  registry.reconcileSelection();
  choices.clear();
  choices.reserve(registry.choices().size() + 1);
  // Empty ID is the original shared /.sleep + /sleep pool.
  choices.push_back({"", "All wallpapers (/.sleep + /sleep)"});
  for (const auto& choice : registry.choices()) choices.push_back({choice.id, choice.label});

  items.clear();
  items.reserve(choices.size());
  int selected = 0;
  for (size_t i = 0; i < choices.size(); ++i) {
    fui::ListItem item;
    item.label = choices[i].label.c_str();
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
    if (choices[i].id == SETTINGS.wallpaperPackId) selected = static_cast<int>(i);
  }
  nav.requestSelection(selected);
  requestUpdate();
}

void WallpaperCollectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void WallpaperCollectionActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  const auto& id = choices[static_cast<size_t>(index)].id;
  if (id.size() >= sizeof(SETTINGS.wallpaperPackId)) return;
  std::memcpy(SETTINGS.wallpaperPackId, id.c_str(), id.size() + 1);
  SETTINGS.saveToFile();
  app.clearTapFlash();
  finish();
}

void WallpaperCollectionActivity::onRowLongPress(const int index) { confirmRemove(index); }

bool WallpaperCollectionActivity::handleButtons() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, 1000)) {
    confirmRemove(nav.selected);
    return true;
  }
  return UiListActivity::handleButtons();
}

void WallpaperCollectionActivity::confirmRemove(const int index) {
  if (index <= 0 || index >= listCount()) return;
  const std::string id = choices[static_cast<size_t>(index)].id;
  const std::string label = choices[static_cast<size_t>(index)].label;
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(
      renderer, mappedInput, "Remove Wallpaper Collection", "Remove " + label + "?",
      tr(STR_CANCEL), "Remove");
  if (!confirmation) return;
  app.clearTapFlash();
  startActivityForResult(std::move(confirmation), [this, id](const ActivityResult& result) {
    if (result.isCancelled) return;
    auto& registry = WallpaperPackRegistry::getInstance();
    registry.scan();
    if (!registry.remove(id)) {
      LOG_ERR("WPK", "Failed to remove wallpaper collection: %s", id.c_str());
      return;
    }
    refreshChoices();
  });
}

