#pragma once

#include <esp_partition.h>
#include <sdkconfig.h>

#include "NorFlash.h"

/// NorFlash over the font area of the `spiffs` partition: everything below the language slot
/// (FlashFontPartition::fontUsableSize()). The TTF glyph store reaches flash only through this class, and
/// it is the only code of that store that calls the ESP-IDF partition API.
///
/// The partition is shared with FlashFontPartition (.cpfont families), so the two exclude each other:
/// program() and erase() are refused while a .cpfont is mapped or being written, and FlashFontPartition
/// refuses to write or map while this class holds a mapping (liveMappings()). Not thread-safe; callers
/// serialise (RenderLock). Destroying it releases any mapping still held.
class EspNorFlash final : public NorFlash {
 public:
  EspNorFlash() = default;
  ~EspNorFlash() override;
  EspNorFlash(const EspNorFlash&) = delete;
  EspNorFlash& operator=(const EspNorFlash&) = delete;

  /// Finds the partition. False when it is missing or not larger than the language slot.
  bool begin();

  uint32_t size() const override { return size_; }
  bool read(uint32_t offset, void* dst, uint32_t len) const override;
  bool program(uint32_t offset, const void* src, uint32_t len) override;
  bool erase(uint32_t offset, uint32_t len) override;
  const uint8_t* map(uint32_t offset, uint32_t len) override;
  void unmap(const uint8_t* mapped) override;
  uint32_t freeMapPages() const override;

  /// Mappings held by every EspNorFlash: FlashFontPartition stays off the partition while this is not 0.
  static uint32_t liveMappings();

 private:
  static constexpr int kMaxMaps = 4;  // the glyph regions, a style (or the copied tables), and spares

  struct Mapping {
    const uint8_t* ptr = nullptr;
    esp_partition_mmap_handle_t handle = 0;
  };

  bool inRange(const uint32_t offset, const uint32_t len) const {
    return part_ != nullptr && offset <= size_ && len <= size_ - offset;
  }
  bool writable(const char* what, uint32_t offset, uint32_t len) const;
  bool write(uint32_t offset, const void* src, uint32_t len);

  const esp_partition_t* part_ = nullptr;
  uint32_t size_ = 0;
  Mapping maps_[kMaxMaps];
#if CONFIG_SPIRAM
  // ESP-IDF writes from external RAM 32 bytes at a time, each a whole flash operation; such sources are
  // copied through this internal buffer instead.
  uint8_t bounce_[512];
#endif
};
