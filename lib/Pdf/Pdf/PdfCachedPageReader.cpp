#include "PdfCachedPageReader.h"

#include <HalStorage.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>

#include "PdfLog.h"

namespace {

constexpr uint8_t kPageVersionV5 = 5;

}  // namespace

void PdfCachedPageReader::close() {
  if (file_.isOpen()) {
    file_.close();
  }
  textOffsets_.clear();
  imageOffsets_.clear();
  drawSteps_.clear();
  textCount_ = 0;
  imageCount_ = 0;
  drawCount_ = 0;
}

bool PdfCachedPageReader::open(const char* cacheDir, uint32_t pageNum) {
  close();
  if (!cacheDir || !cacheDir[0]) {
    return false;
  }

  PdfFixedString<PDF_MAX_PATH> path;
  if (!path.assign(cacheDir, std::strlen(cacheDir)) || !path.append("/pages/", 7) ||
      !appendUnsigned(path, static_cast<size_t>(pageNum)) || !path.append(".bin", 4)) {
    return false;
  }

#ifdef HAL_STORAGE_STUB
  if (!file_.loadPath(path.c_str())) {
    return false;
  }
#else
  if (!Storage.openFileForRead("PDF", path.c_str(), file_)) {
    return false;
  }
#endif

  uint8_t ver = 0;
  serialization::readPod(file_, ver);
  if (ver != kPageVersionV5) {
    pdfLogErr("PdfCachedPageReader: unsupported page cache version");
    close();
    return false;
  }

  serialization::readPod(file_, textCount_);
  if (textCount_ > PDF_MAX_TEXT_BLOCKS) {
    close();
    return false;
  }
  textOffsets_.clear();
  for (uint32_t i = 0; i < textCount_; ++i) {
    const uint32_t start = static_cast<uint32_t>(file_.position());
    if (!textOffsets_.push_back(start)) {
      close();
      return false;
    }
    uint32_t len = 0;
    serialization::readPod(file_, len);
    if (!file_.seek(file_.position() + static_cast<size_t>(len))) {
      close();
      return false;
    }
    uint8_t style = 0;
    uint32_t orderHint = 0;
    serialization::readPod(file_, style);
    serialization::readPod(file_, orderHint);
  }

  serialization::readPod(file_, imageCount_);
  if (imageCount_ > PDF_MAX_IMAGES_PER_PAGE) {
    close();
    return false;
  }
  imageOffsets_.clear();
  for (uint32_t i = 0; i < imageCount_; ++i) {
    const uint32_t start = static_cast<uint32_t>(file_.position());
    if (!imageOffsets_.push_back(start)) {
      close();
      return false;
    }
    uint32_t dummy32 = 0;
    uint16_t dummy16 = 0;
    serialization::readPod(file_, dummy32);
    serialization::readPod(file_, dummy32);
    serialization::readPod(file_, dummy16);
    serialization::readPod(file_, dummy16);
    uint8_t dummy8 = 0;
    serialization::readPod(file_, dummy8);
  }

  serialization::readPod(file_, drawCount_);
  if (drawCount_ > PDF_MAX_DRAW_STEPS) {
    close();
    return false;
  }
  drawSteps_.clear();
  for (uint32_t i = 0; i < drawCount_; ++i) {
    uint8_t im = 0;
    serialization::readPod(file_, im);
    PdfDrawStep step{};
    serialization::readPod(file_, step.index);
    step.isImage = im != 0;
    if (!drawSteps_.push_back(step)) {
      close();
      return false;
    }
  }

  return true;
}

bool PdfCachedPageReader::loadTextBlock(uint32_t index, PdfTextBlock& out) {
  if (index >= textOffsets_.size()) {
    return false;
  }
  if (!file_.seek(textOffsets_[index])) {
    return false;
  }
  out.text.clear();
  uint32_t len = 0;
  serialization::readPod(file_, len);
  if (len >= PDF_MAX_TEXT_BLOCK_BYTES) {
    return false;
  }
  if (!out.text.resize(len)) {
    return false;
  }
  if (len > 0 && file_.read(reinterpret_cast<uint8_t*>(out.text.data()), len) != static_cast<int>(len)) {
    return false;
  }
  out.text.data()[len] = '\0';
  serialization::readPod(file_, out.style);
  serialization::readPod(file_, out.orderHint);
  return true;
}

bool PdfCachedPageReader::loadImage(uint32_t index, PdfImageDescriptor& out) {
  if (index >= imageOffsets_.size()) {
    return false;
  }
  if (!file_.seek(imageOffsets_[index])) {
    return false;
  }
  serialization::readPod(file_, out.pdfStreamOffset);
  serialization::readPod(file_, out.pdfStreamLength);
  serialization::readPod(file_, out.width);
  serialization::readPod(file_, out.height);
  serialization::readPod(file_, out.format);
  return true;
}

bool PdfCachedPageReader::loadDrawStep(uint32_t index, PdfDrawStep& out) const {
  if (index >= drawSteps_.size()) {
    return false;
  }
  out = drawSteps_[index];
  return true;
}
