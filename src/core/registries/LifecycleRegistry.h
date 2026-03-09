#pragma once

#include <cstdint>

class GfxRenderer;

namespace core {

struct LifecycleEntry {
  void (*onStorageReady)();
  void (*onSettingsLoaded)(GfxRenderer& renderer);
  void (*onFontSetup)(GfxRenderer& renderer);
  void (*onFontFamilyChanged)(uint8_t newFontFamilyValue);
  void (*onWebSettingsApplied)();
  void (*onUploadCompleted)(const char* uploadPath, const char* uploadFileName);
};

class LifecycleRegistry {
 public:
  static constexpr int kMaxEntries = 16;

  static void add(const LifecycleEntry& entry) {
    if (count >= kMaxEntries) {
      return;
    }

    entries[count++] = entry;
  }

  static void dispatchStorageReady() {
    for (int i = 0; i < count; ++i) {
      if (entries[i].onStorageReady != nullptr) {
        entries[i].onStorageReady();
      }
    }
  }

  static void dispatchSettingsLoaded(GfxRenderer& renderer) {
    for (int i = 0; i < count; ++i) {
      if (entries[i].onSettingsLoaded != nullptr) {
        entries[i].onSettingsLoaded(renderer);
      }
    }
  }

  static void dispatchFontSetup(GfxRenderer& renderer) {
    for (int i = 0; i < count; ++i) {
      if (entries[i].onFontSetup != nullptr) {
        entries[i].onFontSetup(renderer);
      }
    }
  }

  static void dispatchFontFamilyChanged(uint8_t newValue) {
    for (int i = 0; i < count; ++i) {
      if (entries[i].onFontFamilyChanged != nullptr) {
        entries[i].onFontFamilyChanged(newValue);
      }
    }
  }

  static void dispatchWebSettingsApplied() {
    for (int i = 0; i < count; ++i) {
      if (entries[i].onWebSettingsApplied != nullptr) {
        entries[i].onWebSettingsApplied();
      }
    }
  }

  static void dispatchUploadCompleted(const char* uploadPath, const char* uploadFileName) {
    for (int i = 0; i < count; ++i) {
      if (entries[i].onUploadCompleted != nullptr) {
        entries[i].onUploadCompleted(uploadPath, uploadFileName);
      }
    }
  }

 private:
  inline static LifecycleEntry entries[kMaxEntries] = {};
  inline static int count = 0;
};

}  // namespace core
