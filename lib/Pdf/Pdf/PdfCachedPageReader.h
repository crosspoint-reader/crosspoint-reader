#pragma once

#include <HalStorage.h>

#include "PdfFixed.h"
#include "PdfLimits.h"
#include "PdfPage.h"

class PdfCachedPageReader {
  FsFile file_;
  PdfFixedVector<uint32_t, PDF_MAX_TEXT_BLOCKS> textOffsets_;
  PdfFixedVector<uint32_t, PDF_MAX_IMAGES_PER_PAGE> imageOffsets_;
  PdfFixedVector<PdfDrawStep, PDF_MAX_DRAW_STEPS> drawSteps_;
  uint32_t textCount_ = 0;
  uint32_t imageCount_ = 0;
  uint32_t drawCount_ = 0;

  template <size_t N>
  static bool appendUnsigned(PdfFixedString<N>& s, size_t value) {
    char buf[32];
    size_t len = 0;
    do {
      if (len >= sizeof(buf)) {
        return false;
      }
      buf[len++] = static_cast<char>('0' + (value % 10));
      value /= 10;
    } while (value != 0);
    while (len > 0) {
      if (!s.append(&buf[len - 1], 1)) {
        return false;
      }
      --len;
    }
    return true;
  }

 public:
  PdfCachedPageReader() = default;
  ~PdfCachedPageReader() { close(); }

  PdfCachedPageReader(const PdfCachedPageReader&) = delete;
  PdfCachedPageReader& operator=(const PdfCachedPageReader&) = delete;

  PdfCachedPageReader(PdfCachedPageReader&&) = delete;
  PdfCachedPageReader& operator=(PdfCachedPageReader&&) = delete;

  void close();
  bool open(const char* cacheDir, uint32_t pageNum);

  [[nodiscard]] uint32_t textCount() const { return textCount_; }
  [[nodiscard]] uint32_t imageCount() const { return imageCount_; }
  [[nodiscard]] uint32_t drawCount() const { return drawCount_; }

  bool loadTextBlock(uint32_t index, PdfTextBlock& out);
  bool loadImage(uint32_t index, PdfImageDescriptor& out);
  bool loadDrawStep(uint32_t index, PdfDrawStep& out) const;
};
