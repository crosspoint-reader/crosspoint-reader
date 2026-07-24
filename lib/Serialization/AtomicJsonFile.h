#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "AtomicFile.h"

namespace AtomicJsonFile {

constexpr size_t DEFAULT_MAX_BYTES = 64U * 1024U;

AtomicFile::LoadStatus load(const char* path, std::string& json, size_t maxSize = DEFAULT_MAX_BYTES);
AtomicFile::SaveStatus save(const char* path, const char* json, size_t size,
                            size_t maxSize = DEFAULT_MAX_BYTES);

inline AtomicFile::SaveStatus save(const char* path, const String& json, const size_t maxSize = DEFAULT_MAX_BYTES) {
  return save(path, json.c_str(), json.length(), maxSize);
}

}  // namespace AtomicJsonFile
