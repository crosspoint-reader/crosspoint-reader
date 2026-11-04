#pragma once

#include <cstdint>

namespace freeink::font {
class FtFont {
 public:
  using ReadFn = unsigned long (*)(void*, unsigned long, unsigned char*, unsigned long);
  enum class InspectResult { Ok, Unsupported, Unavailable };
  struct FaceInfo {
    uint16_t weight = 400;
    bool italic = false;
  };

  // Test files encode only weight (big-endian) and the metadata italic flag.
  static InspectResult inspectStream(ReadFn read, void* ctx, unsigned long size, FaceInfo& info) {
    unsigned char bytes[3];
    if (size != sizeof(bytes) || read(ctx, 0, bytes, sizeof(bytes)) != sizeof(bytes)) {
      return InspectResult::Unavailable;
    }
    info.weight = (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1];
    info.italic = bytes[2] != 0;
    return InspectResult::Ok;
  }
};
}  // namespace freeink::font
