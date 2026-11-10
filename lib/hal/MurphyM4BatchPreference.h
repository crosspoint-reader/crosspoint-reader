#pragma once

#include <MurphyM4Batch.h>

#include <cstdint>

namespace MurphyM4BatchPreference {

enum class StoredValue : uint8_t { First = 1, Second = 2 };

constexpr freeink::MurphyM4Batch decode(const uint8_t stored) {
  return stored == static_cast<uint8_t>(StoredValue::First) ? freeink::MurphyM4Batch::First
                                                            : freeink::MurphyM4Batch::Second;
}

constexpr uint8_t encode(const freeink::MurphyM4Batch batch) {
  return static_cast<uint8_t>(batch == freeink::MurphyM4Batch::First ? StoredValue::First : StoredValue::Second);
}

}  // namespace MurphyM4BatchPreference