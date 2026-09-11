#pragma once

#include <cstdint>

#include "CrossPointPosition.h"

enum class ProgressComparison : uint8_t { LocalAhead, Synchronized, RemoteAhead, Unknown };

enum class RemoteRecordChoice : uint8_t { Primary, Alternate };

ProgressComparison compareProgress(const CrossPointPosition& local, float localPercentage,
                                   const CrossPointPosition& remote, float remotePercentage);

RemoteRecordChoice selectRemoteRecord(const CrossPointPosition& primary, float primaryPercentage,
                                      const CrossPointPosition& alternate, float alternatePercentage);
