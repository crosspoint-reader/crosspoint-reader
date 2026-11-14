#pragma once

#include <cstddef>
#include <cstdint>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;

struct esp_partition_t {
  uint32_t size;
};

// Reads from the image the test loaded as the "running" app partition.
esp_err_t esp_partition_read(const esp_partition_t* partition, size_t offset, void* destination, size_t size);
