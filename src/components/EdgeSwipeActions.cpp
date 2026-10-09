#include "EdgeSwipeActions.h"

#include <HalFrontlight.h>

#include "icons/edgeSwipeIcons.h"

namespace edge_swipe {
namespace {
Action back() { return Action::Back; }
Action home() { return Action::Home; }
Action menu() { return Action::Menu; }
Action light() { return Action::Light; }
constexpr ActionConfig ACTIONS[] = {
    {Edge::None, nullptr, StrId::STR_NONE_OPT, nullptr},
    {Edge::Top, &icon_edge_menu_24, StrId::STR_READER_MENU, menu},
    {Edge::Bottom, &icon_edge_home_24, StrId::STR_HOME_SHORTCUT, home},
    {Edge::Left, &icon_edge_back_24, StrId::STR_BACK, back},
};
constexpr ActionConfig LIGHT = {Edge::Top, &icon_edge_light_24, StrId::STR_FRONTLIGHT, light};
}  // namespace

const ActionConfig& actionFor(Edge edge) {
  if (edge == Edge::Top && Frontlight.present()) return LIGHT;
  return ACTIONS[static_cast<unsigned>(edge)];
}
}  // namespace edge_swipe
