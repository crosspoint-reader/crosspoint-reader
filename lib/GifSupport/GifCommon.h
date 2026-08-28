#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <cstring>

struct GifBasicInfo {
  uint16_t canvasWidth{0};
  uint16_t canvasHeight{0};
  uint16_t frameX{0};
  uint16_t frameY{0};
  uint16_t frameWidth{0};
  uint16_t frameHeight{0};
  bool interlaced{false};
};

namespace GifCommon {

inline uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

inline bool skipSubBlocks(HalFile& file) {
  uint8_t blockLen = 0;
  while (true) {
    if (file.read(&blockLen, 1) != 1) return false;
    if (blockLen == 0) return true;
    if (!file.seekCur(blockLen)) return false;
  }
}

inline bool readBasicInfo(HalFile& file, GifBasicInfo& info) {
  info = {};
  if (!file.seek(0)) return false;

  uint8_t header[13];
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    file.seek(0);
    return false;
  }
  const bool isGif87 = memcmp(header, "GIF87a", 6) == 0;
  const bool isGif89 = memcmp(header, "GIF89a", 6) == 0;
  if (!isGif87 && !isGif89) {
    file.seek(0);
    return false;
  }

  info.canvasWidth = readLe16(&header[6]);
  info.canvasHeight = readLe16(&header[8]);
  const uint8_t packed = header[10];
  if ((packed & 0x80) != 0) {
    const uint32_t paletteEntries = 1u << ((packed & 0x07) + 1u);
    if (!file.seekCur(static_cast<int32_t>(paletteEntries * 3u))) {
      file.seek(0);
      return false;
    }
  }

  while (true) {
    uint8_t marker = 0;
    if (file.read(&marker, 1) != 1) {
      file.seek(0);
      return false;
    }

    if (marker == 0x3B) {
      file.seek(0);
      return false;
    }

    if (marker == 0x21) {
      uint8_t label = 0;
      if (file.read(&label, 1) != 1 || !skipSubBlocks(file)) {
        file.seek(0);
        return false;
      }
      continue;
    }

    if (marker == 0x2C) {
      uint8_t descriptor[9];
      if (file.read(descriptor, sizeof(descriptor)) != static_cast<int>(sizeof(descriptor))) {
        file.seek(0);
        return false;
      }
      info.frameX = readLe16(&descriptor[0]);
      info.frameY = readLe16(&descriptor[2]);
      info.frameWidth = readLe16(&descriptor[4]);
      info.frameHeight = readLe16(&descriptor[6]);
      info.interlaced = (descriptor[8] & 0x40) != 0;
      file.seek(0);
      return true;
    }

    file.seek(0);
    return false;
  }
}

}  // namespace GifCommon