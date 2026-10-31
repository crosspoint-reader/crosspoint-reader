#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace wallpaperpackages {
constexpr size_t MAX_MANIFEST_BYTES = 12u * 1024u;
constexpr size_t MAX_ARCHIVE_BYTES = 32u * 1024u * 1024u;

struct Release {
  std::string name, variant, version, url, sha256;
  uint16_t width = 0, height = 0;
};
bool selectRelease(const std::string& path, const std::string& collection, uint16_t width, uint16_t height,
                   Release& release);
bool installVerifiedZip(const std::string& archivePath, const Release& release);
}  // namespace wallpaperpackages
