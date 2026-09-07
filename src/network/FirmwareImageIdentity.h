#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Pure chip-identity logic shared by the two firmware install paths: the SD
// card flasher (FirmwareFlasher) and the network OTA download (OtaUpdater).
// Header-only and free of Arduino/IDF dependencies so the host test suite
// (test/firmware_image_identity) exercises the identical code the firmware
// runs -- the two paths previously carried hand-copied instances of the same
// comparison, which is how they came to be able to drift.
//
// Two distinct things are called a "chip id" here and must not be conflated:
//
//   * The IMAGE's chip id: two little-endian bytes at offset 12 of
//     esp_image_header_t, read out of a candidate .bin. Read it with
//     readImageChipId(), which fails explicitly on a short buffer -- 0x0000 is
//     a legitimate esp_chip_id_t (ESP32), so a zeroed output can never stand in
//     for "could not read".
//
//   * The DEVICE's chip id: what this build was compiled for. Derive it from
//     the build target (CONFIG_IDF_FIRMWARE_CHIP_ID, the very constant the
//     bootloader compares every image against on every boot), never by reading
//     mutable storage. chipIdForModel() is the fallback for a toolchain that
//     does not expose that macro.
namespace firmware_identity {

// ESP_CHIP_ID_INVALID: "no usable identity", never a real chip.
inline constexpr uint16_t CHIP_ID_UNKNOWN = 0xFFFF;

// chip_id's position and the header's total length, both from
// esp_image_header_t. The field is a packed little-endian uint16_t.
inline constexpr size_t CHIP_ID_OFFSET = 12;
inline constexpr size_t IMAGE_HEADER_SIZE = 24;

// esp_chip_id_t values, for the fallback mapping and for callers that need to
// name a family without pulling in esp_app_format.h. FirmwareFlasher.cpp
// static_asserts each of these against the real enum.
inline constexpr uint16_t CHIP_ID_ESP32 = 0x0000;
inline constexpr uint16_t CHIP_ID_ESP32S2 = 0x0002;
inline constexpr uint16_t CHIP_ID_ESP32C3 = 0x0005;
inline constexpr uint16_t CHIP_ID_ESP32S3 = 0x0009;
inline constexpr uint16_t CHIP_ID_ESP32C2 = 0x000C;
inline constexpr uint16_t CHIP_ID_ESP32C6 = 0x000D;
inline constexpr uint16_t CHIP_ID_ESP32H2 = 0x0010;
inline constexpr uint16_t CHIP_ID_ESP32P4 = 0x0012;
inline constexpr uint16_t CHIP_ID_ESP32C61 = 0x0014;
inline constexpr uint16_t CHIP_ID_ESP32C5 = 0x0017;
inline constexpr uint16_t CHIP_ID_ESP32H21 = 0x0019;
inline constexpr uint16_t CHIP_ID_ESP32H4 = 0x001C;

// esp_chip_model_t values. A SEPARATE enum from esp_chip_id_t, and the two
// already disagree: ESP32 is model 1 but chip id 0x0000. The remaining
// families happen to share a number today -- one header writes them in
// decimal, the other in hex, which is why they look unrelated -- but nothing
// in the SDK ties the enums together, and a new part could be added to one
// without the other. The mapping below is therefore an explicit switch, never
// a cast. FirmwareFlasher.cpp static_asserts these against the real enum.
inline constexpr int MODEL_ESP32 = 1;
inline constexpr int MODEL_ESP32S2 = 2;
inline constexpr int MODEL_ESP32S3 = 9;
inline constexpr int MODEL_ESP32C3 = 5;
inline constexpr int MODEL_ESP32C2 = 12;
inline constexpr int MODEL_ESP32C6 = 13;
inline constexpr int MODEL_ESP32H2 = 16;
inline constexpr int MODEL_ESP32P4 = 18;
inline constexpr int MODEL_ESP32C61 = 20;
inline constexpr int MODEL_ESP32C5 = 23;
inline constexpr int MODEL_ESP32H21 = 25;
inline constexpr int MODEL_ESP32H4 = 28;

// Reads a candidate image's chip id out of `header`. False when the buffer is
// too short to contain the field, leaving `out` untouched: the caller must
// treat that as a failure and not as a chip id, because every uint16_t value
// including 0x0000 names a real chip.
inline bool readImageChipId(const uint8_t* header, const size_t len, uint16_t& out) {
  if (header == nullptr) return false;
  if (len < CHIP_ID_OFFSET + sizeof(uint16_t)) return false;
  uint16_t value = 0;
  // memcpy, not a uint16_t* cast: `header` is a byte buffer of unknown
  // alignment and RISC-V faults on an unaligned 16-bit load.
  std::memcpy(&value, header + CHIP_ID_OFFSET, sizeof(value));
  // esp_image_header_t stores chip_id little-endian; every target this builds
  // for is little-endian, so the memcpy is already in host order.
  out = value;
  return true;
}

enum class ChipVerdict : unsigned char {
  Match,                 // image is built for this chip family
  Mismatch,              // image names a different chip family -- refuse
  UnknownDeviceIdentity  // we cannot say what chip this is -- do not refuse
};

// The install guard's decision.
//
// Fail CLOSED on a known mismatch: flashing an image built for another MCU
// family produces a device that cannot boot, and the bootloader would reject
// it anyway, leaving nothing to boot at all.
//
// Fail OPEN when the DEVICE's identity is unknown. A guard that refuses on
// "unknown" bricks the update path itself: a sealed device with no usable USB
// port and a failing identity source refuses every image, including one byte
// identical to the firmware already running, and can never be updated again.
// The bootloader re-checks chip_id against CONFIG_IDF_FIRMWARE_CHIP_ID on
// every boot, so it -- not this pre-flight check -- is the authority that
// actually keeps a wrong-family image from running; the fallback OTA slot
// survives a refused boot.
inline constexpr ChipVerdict compareChipId(const uint16_t imageChip, const uint16_t deviceChip) {
  if (deviceChip == CHIP_ID_UNKNOWN) return ChipVerdict::UnknownDeviceIdentity;
  return imageChip == deviceChip ? ChipVerdict::Match : ChipVerdict::Mismatch;
}

// Maps an esp_chip_model_t value to its esp_chip_id_t. Used only when the
// build target's own CONFIG_IDF_FIRMWARE_CHIP_ID is unavailable.
//
// ESP32 alone already needs the mapping (model 1 -> id 0x0000); the rest are
// spelled out so a future model that stops agreeing is a compile-time table
// entry rather than a silently wrong cast.
//
// An unrecognised model yields CHIP_ID_UNKNOWN, never CHIP_ID_ESP32: 0x0000 is
// a valid id, so returning it for "don't know" would silently claim the device
// is an ESP32 and reject every image built for the chip it actually is.
inline constexpr uint16_t chipIdForModel(const int espChipModel) {
  switch (espChipModel) {
    case MODEL_ESP32:
      return CHIP_ID_ESP32;
    case MODEL_ESP32S2:
      return CHIP_ID_ESP32S2;
    case MODEL_ESP32S3:
      return CHIP_ID_ESP32S3;
    case MODEL_ESP32C3:
      return CHIP_ID_ESP32C3;
    case MODEL_ESP32C2:
      return CHIP_ID_ESP32C2;
    case MODEL_ESP32C6:
      return CHIP_ID_ESP32C6;
    case MODEL_ESP32H2:
      return CHIP_ID_ESP32H2;
    case MODEL_ESP32P4:
      return CHIP_ID_ESP32P4;
    case MODEL_ESP32C61:
      return CHIP_ID_ESP32C61;
    case MODEL_ESP32C5:
      return CHIP_ID_ESP32C5;
    case MODEL_ESP32H21:
      return CHIP_ID_ESP32H21;
    case MODEL_ESP32H4:
      return CHIP_ID_ESP32H4;
    default:
      return CHIP_ID_UNKNOWN;
  }
}

}  // namespace firmware_identity
