#pragma once

#include <cstdint>

enum class EpubRenderMode : uint8_t {
  Full = 0,
  Balanced = 1,
  Light = 2,
};

constexpr uint8_t EPUB_RENDER_MODE_COUNT = 3;

inline bool isValidEpubRenderMode(const uint8_t mode) { return mode < EPUB_RENDER_MODE_COUNT; }
