#pragma once

#include <cstdint>

constexpr uint8_t ESP_IMAGE_HEADER_MAGIC = 0xE9;
constexpr uint8_t ESP_IMAGE_MAX_SEGMENTS = 16;

// Same sizes and leading fields as ESP-IDF's esp_app_format.h.
struct esp_image_header_t {
  uint8_t magic;
  uint8_t segment_count;
  uint8_t unused[22];
};
static_assert(sizeof(esp_image_header_t) == 24, "ESP image header is 24 bytes");

struct esp_image_segment_header_t {
  uint32_t load_addr;
  uint32_t data_len;
};
static_assert(sizeof(esp_image_segment_header_t) == 8, "ESP segment header is 8 bytes");
