#include <HalDisplay.h>
#include <EmulationUtils.h>
#include <string>

HalDisplay::HalDisplay(int8_t sclk, int8_t mosi, int8_t cs, int8_t dc, int8_t rst, int8_t busy) : einkDisplay(sclk, mosi, cs, dc, rst, busy) {
  if (is_emulated) {
    emuFramebuffer0 = new uint8_t[BUFFER_SIZE];
  }
}

HalDisplay::~HalDisplay() {
  if (emuFramebuffer0) {
    delete[] emuFramebuffer0;
    emuFramebuffer0 = nullptr;
  }
}

void HalDisplay::begin() {
  if (!is_emulated) {
    einkDisplay.begin();
  } else {
    Serial.printf("[%lu] [   ] Emulated display initialized\n", millis());
    // no-op
  }
}

void HalDisplay::clearScreen(uint8_t color) const {
  if (!is_emulated) {
    einkDisplay.clearScreen(color);
  } else {
    Serial.printf("[%lu] [   ] Emulated clear screen with color 0x%02X\n", millis(), color);
    memset(emuFramebuffer0, color, BUFFER_SIZE);
  }
}

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool fromProgmem) const {
  if (!is_emulated) {
    einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
  } else {
    Serial.printf("[%lu] [   ] Emulated draw image at (%u, %u) with size %ux%u\n", millis(), x, y, w, h);

    // Calculate bytes per line for the image
    const uint16_t imageWidthBytes = w / 8;

    // Copy image data to frame buffer
    for (uint16_t row = 0; row < h; row++) {
      const uint16_t destY = y + row;
      if (destY >= DISPLAY_HEIGHT)
        break;

      const uint16_t destOffset = destY * DISPLAY_WIDTH_BYTES + (x / 8);
      const uint16_t srcOffset = row * imageWidthBytes;

      for (uint16_t col = 0; col < imageWidthBytes; col++) {
        if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES)
          break;

        if (fromProgmem) {
          emuFramebuffer0[destOffset + col] = pgm_read_byte(&imageData[srcOffset + col]);
        } else {
          emuFramebuffer0[destOffset + col] = imageData[srcOffset + col];
        }
      }
    }
  }
}

void HalDisplay::displayBuffer(RefreshMode mode) {
  if (!is_emulated) {
    einkDisplay.displayBuffer(mode);
  } else {
    Serial.printf("[%lu] [   ] Emulated display buffer with mode %d\n", millis(), static_cast<int>(mode));
    std::string b64 = EmulationUtils::base64_encode(reinterpret_cast<char*>(emuFramebuffer0), BUFFER_SIZE);
    EmulationUtils::sendCmd(EmulationUtils::CMD_DISPLAY, b64.c_str());
    // no response expected
  }
}

void HalDisplay::refreshDisplay(RefreshMode mode, bool turnOffScreen) {
  if (!is_emulated) {
    einkDisplay.refreshDisplay(mode, turnOffScreen);
  } else {
    Serial.printf("[%lu] [   ] Emulated refresh display with mode %d, turnOffScreen %d\n", millis(), static_cast<int>(mode), turnOffScreen);
    // emulated delay
    if (mode == RefreshMode::FAST_REFRESH) {
      delay(500);
    } else if (mode == RefreshMode::HALF_REFRESH) {
      delay(1000);
    } else if (mode == RefreshMode::FULL_REFRESH) {
      delay(2000);
    }
  }
}

void HalDisplay::deepSleep() {
  if (!is_emulated) {
    einkDisplay.deepSleep();
  } else {
    Serial.printf("[%lu] [   ] Emulated deep sleep\n", millis());
    // no-op
  }
}

uint8_t* HalDisplay::getFrameBuffer() const {
  if (!is_emulated) {
    return einkDisplay.getFrameBuffer();
  } else {
    return emuFramebuffer0;
  }
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  if (!is_emulated) {
    einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
  } else {
    Serial.printf("[%lu] [   ] Emulated copy grayscale buffers\n", millis());
    // TODO: not sure what this does
  }
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) {
  if (!is_emulated) {
    einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer);
  } else {
    Serial.printf("[%lu] [   ] Emulated copy grayscale LSB buffers\n", millis());
    // TODO: not sure what this does
  }
}

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) {
  if (!is_emulated) {
    einkDisplay.copyGrayscaleMsbBuffers(msbBuffer);
  } else {
    Serial.printf("[%lu] [   ] Emulated copy grayscale MSB buffers\n", millis());
    // TODO: not sure what this does
  }
}

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) {
  if (!is_emulated) {
    einkDisplay.cleanupGrayscaleBuffers(bwBuffer);
  } else {
    Serial.printf("[%lu] [   ] Emulated cleanup grayscale buffers\n", millis());
    // TODO: not sure what this does
  }
}

void HalDisplay::displayGrayBuffer() {
  if (!is_emulated) {
    einkDisplay.displayGrayBuffer();
  } else {
    Serial.printf("[%lu] [   ] Emulated display gray buffer\n", millis());
    // TODO: not sure what this does
  }
}
