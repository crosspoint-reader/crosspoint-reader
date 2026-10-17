#include "PluginEventAsset.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdint>

namespace plugineventasset {
namespace {
constexpr uint64_t MAX_BYTES = 1024 * 1024;

uint32_t le32(const uint8_t* bytes) {
  uint32_t value = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    value |= static_cast<uint32_t>(bytes[shift / 8]) << shift;
  }
  return value;
}
uint16_t le16(const uint8_t* bytes) { return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8); }
}  // namespace

bool validBmp(const std::string& path, const int width, const int height) {
  if (width < 0 || width > 2048 || height < 0 || height > 3072 || (width == 0) != (height == 0)) return false;
  HalFile file;
  if (!Storage.openFileForRead("PEVT", path, file)) return false;
  const uint64_t size = file.fileSize64();
  if (size < 54 || size > MAX_BYTES) return false;
  uint8_t header[54];
  if (file.read(header, sizeof(header)) != sizeof(header)) return false;
  const uint32_t offset = le32(header + 10);
  const uint32_t dibSize = le32(header + 14);
  const uint32_t rawWidth = le32(header + 18);
  const uint32_t rawHeight = le32(header + 22);
  const uint32_t imageHeight = rawHeight & 0x80000000u ? 0u - rawHeight : rawHeight;
  const uint16_t bpp = le16(header + 28);
  const uint32_t compression = le32(header + 30);
  uint32_t colors = le32(header + 46);
  // Require a 40-byte DIB so the palette offset matches Bitmap::parseHeaders().
  if (le16(header) != 0x4d42 || le32(header + 2) != size || dibSize != 40 || rawWidth == 0 || rawWidth > 2048 ||
      rawHeight == 0x80000000u || imageHeight == 0 || imageHeight > 3072 ||
      (width && rawWidth != static_cast<uint32_t>(width)) || (height && imageHeight != static_cast<uint32_t>(height))) {
    return false;
  }
  // The decoder does not parse BI_BITFIELDS masks; accept BI_RGB only.
  if (compression != 0 || (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)) return false;
  if (colors == 0 && bpp <= 8) colors = 1u << bpp;
  if (colors > 256 || offset < 54 + colors * 4 || offset > size) return false;
  const uint64_t rowBytes = (static_cast<uint64_t>(rawWidth) * bpp + 31) / 32 * 4;
  if (rowBytes * imageHeight > size - offset) return false;

  // Use the sleep-screen decoder; heap allocation keeps its palette off the task stack.
  auto bitmap = makeUniqueNoThrow<Bitmap>(file);
  if (!bitmap) {
    LOG_ERR("PEVT", "OOM checking BMP");
    return false;
  }
  if (bitmap->parseHeaders() != BmpReaderError::Ok) return false;
  if (bitmap->getWidth() != static_cast<int>(rawWidth) || bitmap->getHeight() != static_cast<int>(imageHeight)) {
    return false;
  }
  return true;
}

bool recover(const std::string& dest) {
  const std::string bak = dest + ".bak";
  if (!Storage.exists(bak.c_str())) return true;
  if (Storage.exists(dest.c_str()) && validBmp(dest, 0, 0)) {
    // A valid destination takes precedence over a leftover backup.
    return Storage.remove(bak.c_str());
  }
  if (!validBmp(bak, 0, 0)) {
    LOG_ERR("PEVT", "unusable image backup: %s", bak.c_str());
    return false;
  }
  if (Storage.exists(dest.c_str()) && !Storage.remove(dest.c_str())) return false;
  if (!Storage.rename(bak.c_str(), dest.c_str())) {
    LOG_ERR("PEVT", "image recovery failed: %s", dest.c_str());
    return false;
  }
  return true;
}

bool commit(const std::string& dest, const int width, const int height, const int status) {
  if (status != 200) return false;
  const std::string tmp = dest + ".part";
  if (!validBmp(tmp, width, height) || !recover(dest)) return false;
  const std::string bak = dest + ".bak";
  const bool hadDest = Storage.exists(dest.c_str());
  if (hadDest && !Storage.rename(dest.c_str(), bak.c_str())) return false;
  if (!Storage.rename(tmp.c_str(), dest.c_str())) {
    if (hadDest && !Storage.rename(bak.c_str(), dest.c_str())) {
      LOG_ERR("PEVT", "image rollback pending: %s", bak.c_str());
    }
    return false;
  }
  // If cleanup fails, the next drain verifies dest before discarding bak.
  if (hadDest) Storage.remove(bak.c_str());
  return true;
}

}  // namespace plugineventasset
