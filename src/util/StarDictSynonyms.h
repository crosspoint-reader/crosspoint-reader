#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace StarDictSynonyms {
constexpr uint32_t SAMPLE_INTERVAL = 256;

bool needsIndex(const std::string& basePath);
// Builds a sampled sidecar with a fixed 4 KiB scan buffer. A malformed .syn
// produces a valid disabled sidecar so normal headword lookup remains usable.
bool buildIndex(const std::string& basePath, void (*yieldFn)(void*) = nullptr, void* ctx = nullptr);
bool lookupOrdinal(const std::string& basePath, const char* target, uint32_t& ordinalOut);
}  // namespace StarDictSynonyms
