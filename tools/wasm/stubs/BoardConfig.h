#pragma once
#include_next <BoardConfig.h>

// The pinned simulator predates EEGO A4; previews use X3, X4, and X4 Pro.
namespace BoardConfig {
inline constexpr bool isEegoA4() { return false; }
}  // namespace BoardConfig
