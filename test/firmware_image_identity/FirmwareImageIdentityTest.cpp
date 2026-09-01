#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "src/network/FirmwareImageIdentity.h"

namespace {

using firmware_identity::CHIP_ID_UNKNOWN;
using firmware_identity::ChipVerdict;

// A real 24-byte esp_image_header_t taken from an ESP32-S3 build: magic 0xE9,
// 7 segments, entry point, and chip_id 0x0009 little-endian at offset 12.
// Pins both the offset and the byte order.
constexpr uint8_t GOLDEN_S3_HEADER[firmware_identity::IMAGE_HEADER_SIZE] = {
    0xE9, 0x07, 0x02, 0x4F, 0x90, 0x5F, 0x37, 0x40, 0xEE, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x01,
};

// Header bytes with `chipId` written little-endian at CHIP_ID_OFFSET.
std::vector<uint8_t> headerWithChipId(const uint16_t chipId) {
  std::vector<uint8_t> header(GOLDEN_S3_HEADER, GOLDEN_S3_HEADER + sizeof(GOLDEN_S3_HEADER));
  header[firmware_identity::CHIP_ID_OFFSET] = static_cast<uint8_t>(chipId & 0xFFU);
  header[firmware_identity::CHIP_ID_OFFSET + 1] = static_cast<uint8_t>((chipId >> 8U) & 0xFFU);
  return header;
}

// The device identity source this guard uses: the build target, which has no
// notion of which OTA slot is currently running.
uint16_t buildTargetChipId(int /*runningSlot*/) {
  return firmware_identity::chipIdForModel(firmware_identity::MODEL_ESP32S3);
}

// The identity source this guard replaced: two bytes read back out of the
// running app partition's header in SPI flash. Modelled as a per-slot value so
// the slot-independence test can show the failure mode -- app1 handing back
// something that is neither the right id nor the unknown sentinel.
uint16_t flashDerivedChipId(const int runningSlot) { return runningSlot == 0 ? 0x0009 : 0x0300; }

TEST(FirmwareImageIdentity, GoldenHeaderYieldsS3ChipId) {
  uint16_t chipId = 0;
  ASSERT_TRUE(firmware_identity::readImageChipId(GOLDEN_S3_HEADER, sizeof(GOLDEN_S3_HEADER), chipId));
  EXPECT_EQ(chipId, firmware_identity::CHIP_ID_ESP32S3);
  EXPECT_EQ(chipId, 0x0009);
}

TEST(FirmwareImageIdentity, ChipIdIsLittleEndianAtOffsetTwelve) {
  // 0x0900 differs from 0x0009 only in byte order: a big-endian read would
  // silently swap an S3 image for a nonexistent chip.
  const std::vector<uint8_t> swapped = headerWithChipId(0x0900);
  uint16_t chipId = 0;
  ASSERT_TRUE(firmware_identity::readImageChipId(swapped.data(), swapped.size(), chipId));
  EXPECT_EQ(chipId, 0x0900);
  EXPECT_EQ(swapped[firmware_identity::CHIP_ID_OFFSET], 0x00);
  EXPECT_EQ(swapped[firmware_identity::CHIP_ID_OFFSET + 1], 0x09);
}

TEST(FirmwareImageIdentity, MatchingChipIdAccepted) {
  EXPECT_EQ(firmware_identity::compareChipId(firmware_identity::CHIP_ID_ESP32S3, firmware_identity::CHIP_ID_ESP32S3),
            ChipVerdict::Match);
  EXPECT_EQ(firmware_identity::compareChipId(firmware_identity::CHIP_ID_ESP32C3, firmware_identity::CHIP_ID_ESP32C3),
            ChipVerdict::Match);
}

TEST(FirmwareImageIdentity, CrossFamilyMismatchRejected) {
  // A C3 image on an S3 device (and the reverse). The guard must stay strict
  // here: this is the case it exists for.
  EXPECT_EQ(firmware_identity::compareChipId(/*image=*/0x0005, /*device=*/0x0009), ChipVerdict::Mismatch);
  EXPECT_EQ(firmware_identity::compareChipId(/*image=*/0x0009, /*device=*/0x0005), ChipVerdict::Mismatch);
}

TEST(FirmwareImageIdentity, UnknownImageChipIsStillAMismatch) {
  // 0xFFFF in an image header is not a chip this device can be; the bootloader
  // would refuse it too. Only an unknown DEVICE identity fails open.
  EXPECT_EQ(firmware_identity::compareChipId(CHIP_ID_UNKNOWN, firmware_identity::CHIP_ID_ESP32S3),
            ChipVerdict::Mismatch);
}

TEST(FirmwareImageIdentity, UnknownDeviceIdentityFailsOpen) {
  // Regression guard. When the device's own identity is unavailable the guard
  // must NOT reject: a sealed device with no usable USB port and a failing
  // identity source would otherwise refuse every image -- including one byte
  // identical to the firmware it is already running -- and could never be
  // updated again. Hardening this into a rejection re-creates that brick.
  EXPECT_EQ(firmware_identity::compareChipId(firmware_identity::CHIP_ID_ESP32S3, CHIP_ID_UNKNOWN),
            ChipVerdict::UnknownDeviceIdentity);
  EXPECT_EQ(firmware_identity::compareChipId(firmware_identity::CHIP_ID_ESP32C3, CHIP_ID_UNKNOWN),
            ChipVerdict::UnknownDeviceIdentity);
  EXPECT_EQ(firmware_identity::compareChipId(CHIP_ID_UNKNOWN, CHIP_ID_UNKNOWN), ChipVerdict::UnknownDeviceIdentity);

  // Explicitly: unknown device identity is neither Match nor Mismatch, so no
  // caller can read it as a rejection.
  EXPECT_NE(firmware_identity::compareChipId(firmware_identity::CHIP_ID_ESP32S3, CHIP_ID_UNKNOWN),
            ChipVerdict::Mismatch);
}

TEST(FirmwareImageIdentity, ShortHeaderRejectedExplicitly) {
  // One byte short of the field, and every shorter length: all must fail, and
  // none may leave a chip id behind.
  for (size_t len = 0; len < firmware_identity::CHIP_ID_OFFSET + sizeof(uint16_t); len++) {
    uint16_t chipId = 0xAAAA;  // sentinel: must be untouched on failure
    EXPECT_FALSE(firmware_identity::readImageChipId(GOLDEN_S3_HEADER, len, chipId)) << "len=" << len;
    EXPECT_EQ(chipId, 0xAAAA) << "len=" << len;
  }

  // Exactly enough bytes for the field succeeds, even without a full header.
  uint16_t chipId = 0;
  EXPECT_TRUE(firmware_identity::readImageChipId(GOLDEN_S3_HEADER, firmware_identity::CHIP_ID_OFFSET + sizeof(uint16_t),
                                                 chipId));
  EXPECT_EQ(chipId, firmware_identity::CHIP_ID_ESP32S3);
}

TEST(FirmwareImageIdentity, NullHeaderRejected) {
  uint16_t chipId = 0xAAAA;
  EXPECT_FALSE(firmware_identity::readImageChipId(nullptr, firmware_identity::IMAGE_HEADER_SIZE, chipId));
  EXPECT_EQ(chipId, 0xAAAA);
}

TEST(FirmwareImageIdentity, ShortReadIsNeverASilentZero) {
  // 0x0000 is ESP32's real chip id, so a failed read that defaulted its output
  // to zero would be indistinguishable from a genuine ESP32 image. Prove the
  // two cases are distinguishable: zero reads back as a success carrying
  // CHIP_ID_ESP32, while a short buffer returns false.
  const std::vector<uint8_t> esp32Header = headerWithChipId(firmware_identity::CHIP_ID_ESP32);
  uint16_t chipId = 0xAAAA;
  ASSERT_TRUE(firmware_identity::readImageChipId(esp32Header.data(), esp32Header.size(), chipId));
  EXPECT_EQ(chipId, firmware_identity::CHIP_ID_ESP32);
  EXPECT_EQ(chipId, 0x0000);
  EXPECT_EQ(firmware_identity::compareChipId(chipId, firmware_identity::CHIP_ID_ESP32), ChipVerdict::Match);

  chipId = 0xAAAA;
  EXPECT_FALSE(firmware_identity::readImageChipId(esp32Header.data(), firmware_identity::CHIP_ID_OFFSET, chipId));
  EXPECT_EQ(chipId, 0xAAAA);
}

TEST(FirmwareImageIdentity, ModelToChipIdTable) {
  // The full esp_chip_model_t -> esp_chip_id_t table. The two are separate
  // enums, so every entry is pinned rather than derived.
  EXPECT_EQ(firmware_identity::chipIdForModel(1), 0x0000);   // ESP32
  EXPECT_EQ(firmware_identity::chipIdForModel(2), 0x0002);   // ESP32-S2
  EXPECT_EQ(firmware_identity::chipIdForModel(5), 0x0005);   // ESP32-C3
  EXPECT_EQ(firmware_identity::chipIdForModel(9), 0x0009);   // ESP32-S3
  EXPECT_EQ(firmware_identity::chipIdForModel(12), 0x000C);  // ESP32-C2
  EXPECT_EQ(firmware_identity::chipIdForModel(13), 0x000D);  // ESP32-C6
  EXPECT_EQ(firmware_identity::chipIdForModel(16), 0x0010);  // ESP32-H2
  EXPECT_EQ(firmware_identity::chipIdForModel(18), 0x0012);  // ESP32-P4
  EXPECT_EQ(firmware_identity::chipIdForModel(20), 0x0014);  // ESP32-C61
  EXPECT_EQ(firmware_identity::chipIdForModel(23), 0x0017);  // ESP32-C5
  EXPECT_EQ(firmware_identity::chipIdForModel(25), 0x0019);  // ESP32-H21
  EXPECT_EQ(firmware_identity::chipIdForModel(28), 0x001C);  // ESP32-H4
}

TEST(FirmwareImageIdentity, ModelToChipIdIsNotACast) {
  // ESP32 is the entry that proves the mapping is needed: model 1, chip id
  // 0x0000. A static_cast<esp_chip_id_t>(model) would call it an ESP32-S2's
  // neighbour and reject every genuine ESP32 image.
  EXPECT_EQ(firmware_identity::chipIdForModel(1), firmware_identity::CHIP_ID_ESP32);
  EXPECT_NE(firmware_identity::chipIdForModel(1), 1);

  // The remaining families currently share a number (hex in one SDK header,
  // decimal in the other). That coincidence is asserted, not relied on: if a
  // future SDK breaks it, ModelToChipIdTable is the test that must be updated.
  for (const int model : {2, 5, 9, 12, 13, 16, 18, 20, 23, 25, 28}) {
    EXPECT_EQ(firmware_identity::chipIdForModel(model), static_cast<uint16_t>(model)) << "model=" << model;
  }
}

TEST(FirmwareImageIdentity, UnmappedModelIsUnknownNotEsp32) {
  // 999 is CHIP_POSIX_LINUX; 0, 7 and 999999 are simply not models. None may
  // map to 0x0000, which would claim the device is an ESP32 and then reject
  // every image built for the chip it actually is.
  for (const int model : {0, 3, 4, 7, 999, 999999, -1}) {
    EXPECT_EQ(firmware_identity::chipIdForModel(model), CHIP_ID_UNKNOWN) << "model=" << model;
    EXPECT_NE(firmware_identity::chipIdForModel(model), firmware_identity::CHIP_ID_ESP32) << "model=" << model;
  }

  // And an unmapped model reaches the fail-open branch, not a rejection.
  EXPECT_EQ(firmware_identity::compareChipId(firmware_identity::CHIP_ID_ESP32S3, firmware_identity::chipIdForModel(7)),
            ChipVerdict::UnknownDeviceIdentity);
}

TEST(FirmwareImageIdentity, DeviceIdentityIsSlotIndependent) {
  // The defect this replaces read device identity out of the running app
  // partition, so the answer changed with which OTA slot happened to be
  // active: app0 agreed with the build target while app1 returned a value that
  // was neither correct nor the unknown sentinel, and every image was refused.
  EXPECT_NE(flashDerivedChipId(0), flashDerivedChipId(1));
  EXPECT_EQ(firmware_identity::compareChipId(firmware_identity::CHIP_ID_ESP32S3, flashDerivedChipId(1)),
            ChipVerdict::Mismatch);

  // Build-target identity has no slot input, so both slots yield the same
  // identity and therefore the same verdict for the same image.
  for (const int runningSlot : {0, 1}) {
    const uint16_t deviceChip = buildTargetChipId(runningSlot);
    EXPECT_EQ(deviceChip, firmware_identity::CHIP_ID_ESP32S3) << "slot app" << runningSlot;
    EXPECT_EQ(firmware_identity::compareChipId(firmware_identity::CHIP_ID_ESP32S3, deviceChip), ChipVerdict::Match)
        << "slot app" << runningSlot;
    EXPECT_EQ(firmware_identity::compareChipId(firmware_identity::CHIP_ID_ESP32C3, deviceChip), ChipVerdict::Mismatch)
        << "slot app" << runningSlot;
  }
  EXPECT_EQ(buildTargetChipId(0), buildTargetChipId(1));
}

TEST(FirmwareImageIdentity, GoldenHeaderInstallsOnItsOwnBuildTarget) {
  // End to end: the exact bytes of an image this device already runs must be
  // accepted from either slot. Refusing this is the field symptom.
  uint16_t imageChip = 0;
  ASSERT_TRUE(firmware_identity::readImageChipId(GOLDEN_S3_HEADER, sizeof(GOLDEN_S3_HEADER), imageChip));
  for (const int runningSlot : {0, 1}) {
    EXPECT_EQ(firmware_identity::compareChipId(imageChip, buildTargetChipId(runningSlot)), ChipVerdict::Match)
        << "slot app" << runningSlot;
  }
}

}  // namespace
