#pragma once
#include <cstdint>

namespace fake {
inline int64_t now = 1800000000;
}
namespace trustedtime {
inline int64_t trustedNow() { return fake::now; }
}  // namespace trustedtime
