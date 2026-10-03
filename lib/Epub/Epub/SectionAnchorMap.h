#pragma once

#include <HalStorage.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sectionAnchors {

// XML IDs cannot contain U+0001. Older readers ignore this key as an unrelated ID.
inline constexpr uint8_t MISSING_PREFIX = 1;

struct Lookup {
  std::optional<uint16_t> page;
  bool checked = false;
};

// The existing string/page encoding also carries one checked-but-missing ID.
inline bool writeEntry(HalFile& file, const std::string& anchor, const uint16_t page, const bool missing = false) {
  if (anchor.size() >= UINT32_MAX) return false;
  const uint32_t length = static_cast<uint32_t>(anchor.size()) + (missing ? 1 : 0);
  return file.write(&length, sizeof(length)) == sizeof(length) &&
         (!missing || file.write(&MISSING_PREFIX, sizeof(MISSING_PREFIX)) == sizeof(MISSING_PREFIX)) &&
         file.write(anchor.data(), anchor.size()) == anchor.size() && file.write(&page, sizeof(page)) == sizeof(page);
}

inline bool write(HalFile& file, const std::vector<std::pair<std::string, uint16_t>>& anchors,
                  const std::string& requested, const uint16_t writtenPages, const bool partial) {
  bool requestedFound = requested.empty();
  uint16_t count = 0;
  for (const auto& [anchor, page] : anchors) {
    if (partial && page >= writtenPages) continue;
    if (count == UINT16_MAX) return false;
    ++count;
    if (anchor == requested) requestedFound = true;
  }
  // A partial has not searched the whole XHTML and cannot certify absence.
  const bool recordMissing = !partial && !requestedFound && count < UINT16_MAX;
  if (recordMissing) ++count;
  if (file.write(&count, sizeof(count)) != sizeof(count)) return false;
  for (const auto& [anchor, page] : anchors) {
    if (partial && page >= writtenPages) continue;
    if (!writeEntry(file, anchor, page)) return false;
  }
  return !recordMissing || writeEntry(file, requested, 0, true);
}

// Read one key at a time without allocating storage for converter-provided IDs.
inline Lookup read(HalFile& file, const std::string& anchor) {
  uint16_t count = 0;
  if (file.read(&count, sizeof(count)) != sizeof(count)) return {};
  for (uint16_t i = 0; i < count; ++i) {
    uint32_t length = 0;
    if (file.read(&length, sizeof(length)) != sizeof(length)) return {};
    const auto available = file.available();
    if (available < static_cast<int>(sizeof(uint16_t)) || length > available - sizeof(uint16_t)) return {};
    uint8_t first = 0;
    if (length > 0 && file.read(&first, sizeof(first)) != sizeof(first)) return {};
    const bool missing = length > 0 && first == MISSING_PREFIX;
    bool matches = !anchor.empty() && length - (missing ? 1u : 0u) == anchor.size();
    if (matches && !missing) matches = first == static_cast<uint8_t>(anchor.front());
    size_t compared = missing ? 0 : 1;
    uint32_t remaining = length > 0 ? length - 1 : 0;
    char chunk[32];
    while (remaining > 0) {
      const size_t bytes = std::min<size_t>(remaining, sizeof(chunk));
      if (file.read(chunk, bytes) != bytes) return {};
      if (matches && std::memcmp(chunk, anchor.data() + compared, bytes) != 0) matches = false;
      compared += bytes;
      remaining -= bytes;
    }
    uint16_t page = 0;
    if (file.read(&page, sizeof(page)) != sizeof(page)) return {};
    if (matches) return {missing ? std::nullopt : std::optional<uint16_t>{page}, true};
  }
  return {};
}

}  // namespace sectionAnchors
