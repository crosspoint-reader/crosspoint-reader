#pragma once

namespace BoardConfig {
struct ViewableInsets {
  int top, right, bottom, left;
};
struct Profile {
  ViewableInsets viewableInsets;
};
inline constexpr Profile ACTIVE = {};
}  // namespace BoardConfig
