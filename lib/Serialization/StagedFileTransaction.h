#pragma once

#include <cstdint>

namespace StagedFileTransaction {

using Validator = bool (*)(const char* path, void* context);

enum class Status : uint8_t { Published, Recovered, NoRecoveryNeeded, InvalidStaging, IoError };

Status recover(const char* finalPath, const char* backupPath, Validator validator, void* context = nullptr);
Status publish(const char* finalPath, const char* stagingPath, const char* backupPath, Validator validator,
               void* context = nullptr);

}  // namespace StagedFileTransaction
