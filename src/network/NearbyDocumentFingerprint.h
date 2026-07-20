#pragma once

#include <string>

// Full-file fingerprint used only by Nearby Position Sync. Unlike KOReader's
// sampled document ID, this reads every byte so optimized or split variants do
// not accidentally share positions merely because their filenames match.
std::string calculateNearbyDocumentFingerprint(const std::string& path);
