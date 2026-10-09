#pragma once

#include <I18n.h>
#include <Icon.h>

#include "util/EdgeSwipe.h"

namespace edge_swipe {
enum class Action : uint8_t { None, Back, Home, Menu, Light };
struct ActionConfig {
  Edge edge;
  const freeink::Icon* icon;
  StrId label;
  Action (*callback)();
};
const ActionConfig& actionFor(Edge edge);
}  // namespace edge_swipe
