#pragma once

#include <cstdint>

namespace BoardConfig {
struct ViewableInsets {
  int16_t top = 0;
  int16_t right = 0;
  int16_t bottom = 0;
  int16_t left = 0;
};
struct Profile {
  ViewableInsets viewableInsets{};
};
inline constexpr Profile ACTIVE{};
}  // namespace BoardConfig
