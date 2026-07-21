#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "activities/reader/GlobalReadingStats.h"

namespace NearbyStatsStorage {

struct Inspection {
  bool protectedStorage = false;
  bool regresses = false;
  bool rotateCanonical = false;
};

// Inspects both the CrossVi envelope triplet and retained CrossInk raw triplet.
// The raw files are never modified, but they still participate in monotonicity
// and newer/I/O protection so migration cannot silently hide more data.
Inspection inspect(const std::string& canonicalPath, const std::string& legacyPath, const GlobalReadingStats& incoming);

// Nearby continues to exchange the exact raw CrossInk v3 payload. Only this
// at-rest publication wraps it as a PeerGlobal CrossVi envelope.
bool saveRawSnapshot(const std::string& canonicalPath, const std::string& legacyPath, const uint8_t* rawPayload,
                     size_t rawPayloadSize, Inspection* inspection = nullptr);

}  // namespace NearbyStatsStorage
