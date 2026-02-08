#include "FsHelpers.h"

#include <BitmapHelpers.h>
#include <SDCardManager.h>

#include <string>
#include <vector>

std::string FsHelpers::normalisePath(const std::string& path) {
  std::vector<std::string> components;
  std::string component;

  for (const auto c : path) {
    if (c == '/') {
      if (!component.empty()) {
        if (component == "..") {
          if (!components.empty()) {
            components.pop_back();
          }
        } else {
          components.push_back(component);
        }
        component.clear();
      }
    } else {
      component += c;
    }
  }

  if (!component.empty()) {
    components.push_back(component);
  }

  std::string result;
  for (const auto& c : components) {
    if (!result.empty()) {
      result += "/";
    }
    result += c;
  }

  return result;
}

bool FsHelpers::saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width, int height) {
  if (!framebuffer) {
    return false;
  }

  std::string path(filename);
  size_t last_slash = path.find_last_of('/');
  if (last_slash != std::string::npos) {
    std::string dir = path.substr(0, last_slash);
    if (!SdMan.exists(dir.c_str())) {
      if (!SdMan.mkdir(dir.c_str())) {
        return false;
      }
    }
  }

  FsFile file;
  if (!SdMan.openFileForWrite("SCR", filename, file)) {
    Serial.printf("[%lu] [SCR] Failed to open file for writing\n", millis());
    return false;
  }

  uint32_t headerSize = 0;
  uint8_t* bmpHeader = createBmpHeader(width, height, &headerSize);
  if (!bmpHeader) {
    file.close();
    SdMan.remove(filename);
    return false;
  }

  bool write_error = false;
  if (file.write(bmpHeader, headerSize) != headerSize) {
    write_error = true;
  }
  delete[] bmpHeader;

  if (write_error) {
    file.close();
    SdMan.remove(filename);
    return false;
  }

  const uint32_t rowSize = (width + 31) / 32 * 4;
  const uint32_t fbRowSize = width / 8;
  const uint32_t paddingSize = rowSize - fbRowSize;
  uint8_t padding[4] = {0, 0, 0, 0};

  for (int y = 0; y < height; y++) {
    const uint8_t* fbRow = framebuffer + (height - 1 - y) * fbRowSize;
    if (file.write(fbRow, fbRowSize) != fbRowSize) {
      write_error = true;
      break;
    }
    if (paddingSize > 0) {
      if (file.write(padding, paddingSize) != paddingSize) {
        write_error = true;
        break;
      }
    }
  }

  file.close();

  if (write_error) {
    SdMan.remove(filename);
    return false;
  }

  return true;
}
