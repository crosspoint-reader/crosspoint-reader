#pragma once

#include "PdfFixed.h"
#include "PdfLimits.h"

#include <cstdint>

struct PdfTextBlock {
  PdfFixedString<PDF_MAX_TEXT_BLOCK_BYTES> text;
  uint32_t orderHint = 0;
};

struct PdfImageDescriptor {
  uint32_t pdfStreamOffset = 0;
  uint32_t pdfStreamLength = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t format = 0;  // 0=JPEG, 1=PNG/FlateDecode
};

struct PdfDrawStep {
  bool isImage = false;
  uint32_t index = 0;
};

struct PdfPage {
  PdfFixedVector<PdfTextBlock, PDF_MAX_TEXT_BLOCKS> textBlocks;
  PdfFixedVector<PdfImageDescriptor, PDF_MAX_IMAGES_PER_PAGE> images;
  PdfFixedVector<PdfDrawStep, PDF_MAX_DRAW_STEPS> drawOrder;

  void clear() {
    textBlocks.clear();
    images.clear();
    drawOrder.clear();
  }
};
