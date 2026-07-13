#include "EndOfBookOptions.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/NextBookFinder.h"

namespace {
// Display name without the file extension, mirroring the file browser rows
std::string displayName(const std::string& filename) {
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}
}  // namespace

void EndOfBookOptions::loadOnce(const std::string& currentBookPath) {
  if (isLoaded.load(std::memory_order_acquire)) {
    return;
  }
  folder = FsHelpers::extractFolderPath(currentBookPath);
  names = NextBookFinder::findNextBooks(currentBookPath, MAX_SUGGESTIONS);
  selector = 0;
  // Release-publish so the main task, which gates all access on isLoaded, never
  // observes a partially built list
  isLoaded.store(true, std::memory_order_release);
}

bool EndOfBookOptions::menuActive() const { return isLoaded.load(std::memory_order_acquire) && !names.empty(); }

std::string EndOfBookOptions::fullPath(const size_t index) const {
  if (index >= names.size()) {
    return {};
  }
  return folder == "/" ? "/" + names[index] : folder + "/" + names[index];
}

EndOfBookOptions::Action EndOfBookOptions::handleMenuInput(const MappedInputManager& input, std::string* openPath) {
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

void EndOfBookOptions::render(GfxRenderer& renderer, const MappedInputManager& input) const {
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
  const int listTop = subtitleY + renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing * 2;

  UITheme::drawCenteredText(renderer, safe, UI_12_FONT_ID, titleY, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, subtitleY, tr(STR_EOB_CONTINUE_WITH));

  const int listHeight = safe.y + safe.height - listTop - metrics.verticalSpacing;
  GUI.drawList(renderer, Rect{safe.x, listTop, safe.width, listHeight}, static_cast<int>(names.size()) + 1, selector,
               [this](const int index) {
                 return index < static_cast<int>(names.size()) ? displayName(names[index])
                                                               : std::string(tr(STR_EOB_HOME));
               });

  const auto labels = input.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
