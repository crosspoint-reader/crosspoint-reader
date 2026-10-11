#include "TouchExitButton.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "TouchUi.h"
#include "components/UITheme.h"
#include "components/themes/TapTargets.h"
#include "fontIds.h"

namespace TouchExitButton {

#if CP_TOUCH_UI

namespace {

constexpr int kExitAction = 0;
constexpr int kBorderPx = 2;
constexpr int kCornerRadius = 8;

bool touchUsable(const MappedInputManager& input) { return input.hasTouch() && input.touchEventsEnabled(); }

}  // namespace

int draw(GfxRenderer& renderer, const MappedInputManager& input, const int x, const int y, const int width) {
  if (!touchUsable(input)) return y;

  // The theme's menu row height: tall enough to hit with a thumb, and the size every other row a
  // finger is expected to press on these screens already has.
  const int height = UITheme::getInstance().getMetrics().menuRowHeight;
  renderer.drawRoundedRect(x, y, width, height, kBorderPx, kCornerRadius, true);

  const char* label = tr(STR_EXIT);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  const int textY = y + (height - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, x + (width - textWidth) / 2, textY, label, true, EpdFontFamily::BOLD);

  TapTargets::Recorder::Builder targets;
  targets.add(x, y, width, height, kExitAction);
  TapTargets::screenActions().record(targets);
  return y + height;
}

void forget() { TapTargets::screenActions().invalidate(); }

bool tapped(const MappedInputManager& input) {
  if (!touchUsable(input)) return false;
  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) return false;
  if (TapTargets::screenActions().hitTest(x, y) != kExitAction) return false;
  LOG_DBG("TCH", "Exit button tap at (%d,%d)", x, y);
  return true;
}

#else  // !CP_TOUCH_UI: no digitiser, so no button and nothing to tap.

int draw(GfxRenderer&, const MappedInputManager&, const int, const int y, const int) { return y; }
void forget() {}
bool tapped(const MappedInputManager&) { return false; }

#endif

}  // namespace TouchExitButton
