#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace AtomicFile {

using Validator = bool (*)(const uint8_t* data, size_t size, void* context);

enum class LoadStatus : uint8_t { Primary, Backup, Temp, Missing, Invalid, Oversize, IoError };
enum class SaveStatus : uint8_t { Saved, Unchanged, Oversize, InvalidExistingState, IoError };

LoadStatus load(const char* path, std::string& data, size_t maxSize, Validator validator, void* context = nullptr);

SaveStatus save(const char* path, const uint8_t* data, size_t size, size_t maxSize, Validator validator,
                void* context = nullptr);

inline SaveStatus save(const char* path, const std::string& data, size_t maxSize, Validator validator,
                       void* context = nullptr) {
  return save(path, reinterpret_cast<const uint8_t*>(data.data()), data.size(), maxSize, validator, context);
}

}  // namespace AtomicFile
