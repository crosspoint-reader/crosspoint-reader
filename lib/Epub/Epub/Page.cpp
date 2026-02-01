#include "Page.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <GfxRenderer.h>

namespace {
constexpr uint16_t MAX_PATH_LEN = 512;
}  // namespace

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset);
}

bool PageLine::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto tb = TextBlock::deserialize(file);
  return std::unique_ptr<PageLine>(new PageLine(std::move(tb), xPos, yPos));
}

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  (void)fontId;
  FsFile file;
  if (!SdMan.openFileForRead("PGE", bmpPath, file)) {
    Serial.printf("[%lu] [PGE] Failed to open image %s\n", millis(), bmpPath.c_str());
    return;
  }

  Bitmap bitmap(file, true);
  const auto err = bitmap.parseHeaders();
  if (err != BmpReaderError::Ok) {
    Serial.printf("[%lu] [PGE] Failed to parse bitmap %s: %s\n", millis(), bmpPath.c_str(),
                  Bitmap::errorToString(err));
    file.close();
    return;
  }

  renderer.drawBitmap(bitmap, xPos + xOffset, yPos + yOffset, 0, 0);
  file.close();
}

bool PageImage::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  const uint16_t pathLen = static_cast<uint16_t>(std::min(bmpPath.size(), static_cast<size_t>(MAX_PATH_LEN)));
  serialization::writePod(file, pathLen);
  return file.write(bmpPath.data(), pathLen) == pathLen;
}

std::unique_ptr<PageImage> PageImage::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  uint16_t pathLen;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);
  serialization::readPod(file, pathLen);

  if (pathLen == 0 || pathLen > MAX_PATH_LEN) {
    Serial.printf("[%lu] [PGE] Invalid image path length: %u\n", millis(), pathLen);
    return nullptr;
  }

  std::string path;
  path.resize(pathLen);
  if (file.read(&path[0], pathLen) != pathLen) {
    Serial.printf("[%lu] [PGE] Failed to read image path\n", millis());
    return nullptr;
  }

  return std::unique_ptr<PageImage>(new PageImage(std::move(path), xPos, yPos));
}

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  for (auto& element : elements) {
    element->render(renderer, fontId, xOffset, yOffset);
  }
}

bool Page::serialize(FsFile& file) const {
  const uint16_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    serialization::writePod(file, static_cast<uint8_t>(el->tag()));
    if (!el->serialize(file)) {
      return false;
    }
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(FsFile& file) {
  auto page = std::unique_ptr<Page>(new Page());

  uint16_t count;
  serialization::readPod(file, count);

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    serialization::readPod(file, tag);

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      if (!pl) {
        return nullptr;
      }
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      if (!pi) {
        return nullptr;
      }
      page->elements.push_back(std::move(pi));
    } else {
      Serial.printf("[%lu] [PGE] Deserialization failed: Unknown tag %u\n", millis(), tag);
      return nullptr;
    }
  }

  return page;
}
