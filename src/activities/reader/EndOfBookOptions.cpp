#include "EndOfBookOptions.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "ReaderUtils.h"
// ReaderUtils.h pulls in ActivityManager.h, which only forward-declares Activity while holding
// std::unique_ptr<Activity> members. Destroying that unique_ptr needs the complete type, so the
// definition must be visible here.
#include "activities/Activity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/NextBookFinder.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;

// Display name without the file extension, mirroring the file browser rows
std::string displayName(const std::string& filename) {
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}
}  // namespace

EndOfBookOptions::EndOfBookOptions(GfxRenderer& renderer) : UiAppHost(renderer), renderer(renderer) {}

void EndOfBookOptions::loadOnce(const std::string& currentBookPath) {
  if (isLoaded.load(std::memory_order_acquire)) {
    return;
  }
  folder = FsHelpers::extractFolderPath(currentBookPath);
  names = NextBookFinder::findNextBooks(currentBookPath, MAX_SUGGESTIONS);
  selector = 0;
  if (!names.empty()) {
    // One-time app setup on the render task, before the first render/route.
    resetUi();
    app.on(ACTION_ROW, &EndOfBookOptions::onRowEvent, this);
    app.setScreen(&EndOfBookOptions::listScreen, this);
    buildRowItems();
  }
  // Release-publish so the main task, which gates all access on isLoaded, never
  // observes a partially built list (rowItems/rowLabels included)
  isLoaded.store(true, std::memory_order_release);
}

// Populates rowLabels/rowItems from names + the trailing "Home" row. Called
// once here since names never changes after loadOnce() completes.
void EndOfBookOptions::buildRowItems() {
  rowCount = 0;
  for (const auto& name : names) {
    if (rowCount >= MAX_ROWS) break;
    rowLabels[rowCount] = displayName(name);
    fui::ListItem item;
    item.label = rowLabels[rowCount].c_str();
    item.actionValue = static_cast<int16_t>(rowCount);
    rowItems[rowCount] = item;
    rowCount++;
  }
  if (rowCount < MAX_ROWS) {
    rowLabels[rowCount] = tr(STR_EOB_HOME);
    fui::ListItem item;
    item.label = rowLabels[rowCount].c_str();
    item.actionValue = static_cast<int16_t>(rowCount);
    rowItems[rowCount] = item;
    rowCount++;
  }
}

bool EndOfBookOptions::menuActive() const { return isLoaded.load(std::memory_order_acquire) && !names.empty(); }

std::string EndOfBookOptions::fullPath(const size_t index) const {
  if (index >= names.size()) {
    return {};
  }
  return folder == "/" ? "/" + names[index] : folder + "/" + names[index];
}

void EndOfBookOptions::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EndOfBookOptions*>(user);
  if (event.value < 0 || event.value > static_cast<int16_t>(self->names.size())) return;
  self->selector = event.value;
  // The tapped row leaves this screen (open book or home); a lingering flash
  // would gray an unrelated element on the next render.
  self->app.clearTapFlash();
  self->tappedRow = event.value;
}

EndOfBookOptions::Action EndOfBookOptions::handleMenuInput(const MappedInputManager& input, std::string* openPath) {
  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent record the tapped row.
  tappedRow = -1;
  const auto route = routeTouch(input);
  // cppcheck can't see that route() dispatches into onRowEvent (registered via
  // app.on(ACTION_ROW, ...)), which sets tappedRow, so it flags this as always false.
  // cppcheck-suppress knownConditionTrueFalse
  if (route && tappedRow >= 0) {
    if (tappedRow < static_cast<int>(names.size())) {
      if (openPath) {
        *openPath = fullPath(tappedRow);
      }
      return Action::OpenBook;
    }
    return Action::GoHome;  // "Home" row tapped
  }
  if (route.routed && app.invalidated()) {
    return Action::Redraw;
  }

  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selector < static_cast<int>(names.size())) {
      if (openPath) {
        *openPath = fullPath(selector);
      }
      return Action::OpenBook;
    }
    return Action::GoHome;  // "Home" entry selected
  }

  // Short-press Back returns to the last page; a long press falls through to the
  // reader's own handler (file browser). Home is reached through the list's Home entry.
  if (input.wasReleased(MappedInputManager::Button::Back) && input.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    return Action::LastPage;
  }

  // Selection movement on the standard list navigation buttons (side Up/Down plus front
  // Left/Right, orientation swap included). It follows the reader's page-turn semantics
  // (press-triggered by default, release-triggered when a long-press behavior is
  // configured, same rule as ReaderUtils::detectPageTurn). This matters on entry: with
  // press-triggered turns, the press that turned the final page already fired in the
  // reader, and its release must not double-fire into this menu.
  const bool usePress = SETTINGS.longPressButtonBehavior == CrossPointSettings::OFF;
  const auto triggered = [&](const MappedInputManager::Button button) {
    return usePress ? input.wasPressed(button) : input.wasReleased(button);
  };
  const int itemCount = static_cast<int>(names.size()) + 1;  // + "Home" entry
  if (triggered(MappedInputManager::Button::NavPrevious)) {
    selector = ButtonNavigator::previousIndex(selector, itemCount);  // wraps to the bottom
    return Action::Redraw;
  }
  if (triggered(MappedInputManager::Button::NavNext)) {
    selector = ButtonNavigator::nextIndex(selector, itemCount);  // wraps to the top
    return Action::Redraw;
  }
  return Action::None;
}

void EndOfBookOptions::listScreen(UiScreen& screen, void* user) {
  static_cast<EndOfBookOptions*>(user)->buildListScreen(screen);
}

void EndOfBookOptions::buildListScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Same layout math as render(): the list band starts under the title/subtitle it
  // draws, and stops above the button hints (the safe-area bottom edge).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int titleY = safe.y + safe.height / 8;
  const int subtitleY = titleY + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;
  const int listTop = subtitleY + renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing * 2;
  screen.setContentMargin(fui::Insets{
      static_cast<int16_t>(listTop), static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.verticalSpacing),
      static_cast<int16_t>(safe.x)});

  // rowLabels/rowItems were built once in loadOnce() (see buildRowItems())
  // and reused here on every repaint.
  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(rowCount);
  props.selectedIndex = static_cast<int16_t>(selector);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in handleMenuInput()
  if (!gpio.hasTouch()) {
    // Non-touch hardware (X3/X4) keeps the original, denser row height
    // instead of FreeInkUI's touch-target-sized default. This short, fixed
    // menu never scrolls, so there's no viewport to resync here. No
    // MappedInputManager reference here (this class isn't an Activity), so
    // this reads the capability directly like BaseTheme's draw code does.
    props.rowHeight = static_cast<int16_t>(metrics.listRowHeight);
  }
  screen.list(props);
}

void EndOfBookOptions::render(GfxRenderer& renderer, const MappedInputManager& input) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (!menuActive()) {
    // No suggestions: the historical plain end screen. 3/8 of the screen height matches
    // the previous fixed position on the 480x800 panel and scales to other resolutions.
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() * 3 / 8, tr(STR_END_OF_BOOK), true,
                              EpdFontFamily::BOLD);
    return;
  }

  // Suggestion menu: title, list (+ Home entry) and button hints. The hints are drawn at
  // the physical front buttons, which is a logical side/top edge in the rotated
  // orientations — lay out inside the safe area so nothing hides behind them. Vertical
  // positions derive from the safe-area height and font line heights so other panel
  // resolutions scale (review request on #2532).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int titleY = safe.y + safe.height / 8;
  const int subtitleY = titleY + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;

  UITheme::drawCenteredText(renderer, safe, UI_12_FONT_ID, titleY, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, subtitleY, tr(STR_EOB_CONTINUE_WITH));

  // The list renders through the FreeInkApp so its rows register touch hit
  // rects; renderUi re-derives the device context, picking up any rotation
  // since construction (reader menu rotate).
  renderUi();

  const auto labels = input.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
