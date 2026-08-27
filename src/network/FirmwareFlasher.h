#pragma once

#include <cstddef>
#include <cstdint>

// Flash a firmware image from an SD-card path into the next OTA app
// partition, then switch otadata so the X3/X4 stock bootloader picks it up
// on next boot. Mirrors the web flasher: raw esp_partition_erase_range +
// esp_partition_write + ota_boot::switchTo (no Arduino Update class, no
// esp_image_verify — those reject our patched image on X4 silicon).
//
// Both the SD update activity and the OTA path land here. OTA first
// downloads the firmware to an SD-card cache file, then calls this.

namespace firmware_flash {

enum class Result {
  OK,
  OPEN_FAIL,
  TOO_SMALL,
  TOO_LARGE,
  BAD_MAGIC,
  BAD_SEGMENTS,         // segment table malformed or runs past EOF
  BAD_CHECKSUM,         // ESP image XOR checksum mismatch
  BAD_SHA,              // SHA256 trailer mismatch (hash_appended images)
  BAD_CHIP,             // image chip_id doesn't match the running MCU family
  WRONG_BOARD,          // image carries a board tag naming a different board
  BAD_SIZE,             // total length doesn't match either the plain-image or the
                        // plain-image-plus-one-Secure-Boot-V2-signature-sector size
  BAD_SIGNATURE_BLOCK,  // a trailing signature sector is present (size matches
                        // that layout) but no slot in it is a structurally
                        // valid, digest-consistent Secure Boot V2 block --
                        // see validateImageFile()'s doc comment
  NO_PARTITION,
  OOM,
  READ_FAIL,
  ERASE_FAIL,
  WRITE_FAIL,
  OTADATA_FAIL,
};

// Progress callback: called after every chunk write. `written`/`total` are bytes.
using ProgressCb = void (*)(size_t written, size_t total, void* ctx);

// Open `sdPath`, validate it looks like an ESP32 image, then stream it into the
// next OTA app partition with interleaved 64 KiB erase + sector writes. On
// success switches otadata via ota_boot::switchTo. Caller is responsible for
// ESP.restart() afterwards.
//
// `alreadyValidated` lets callers that have just run `validateImageFile()`
// themselves (e.g. SdFirmwareUpdateActivity, which validates before showing
// the user the confirmation prompt) skip the redundant second pass. Defaults
// to false so callers without prior validation (any future entry point) keep
// the defense-in-depth check.
Result flashFromSdPath(const char* sdPath, ProgressCb onProgress, void* ctx, bool alreadyValidated = false);

// Full-image integrity check that mirrors the bootloader's verification:
// header magic, segment table walk, XOR checksum, and SHA256 trailer (when
// hash_appended == 1). Also scans for the embedded board tag (see
// FirmwareBoardTag.h) and rejects an image tagged for a different board.
// Run this before flashing a candidate firmware so a truncated/corrupted/
// wrong-board .bin never reaches otadata.
//
// Accepts exactly two total-length layouts:
//   1. Plain image: file ends where the segment/checksum/hash walk above
//      computes it should (unchanged, pre-existing behavior).
//   2. Plain image + one ESP-IDF Secure Boot V2 signature sector: the plain
//      image, 0xFF flash-sector-alignment padding up to the next 4 KiB
//      boundary, then exactly one `ets_secure_boot_signature_t` (4096 bytes,
//      `esp/rom/secure_boot.h`). At least one of its `SECURE_BOOT_NUM_BLOCKS`
//      slots must be a structurally valid block -- correct magic byte, CRC32
//      over the block (matching the exact check `secure_boot_signatures_app.c`'s
//      own `validate_signature_block()` performs), a recognized Secure Boot V2
//      scheme in the version field, and an embedded image digest matching a
//      fresh SHA-256 of the complete padded image. Slots that aren't a valid
//      block (unused, or anything else) are simply not counted, exactly like
//      the real verifier's own tolerant per-slot handling -- this doesn't
//      assume any particular number of populated vs. unused slots.
//   This is structural/integrity validation only, not cryptographic trust
//   verification: it never checks the RSA/ECDSA signature bytes against any
//   public key, so it cannot distinguish "signed with the right key" from
//   "signed with any key" -- see this function's implementation comment for
//   why that's out of scope here.
//
// `partitionSize` is the size of the destination OTA partition; pass 0 to
// skip the size-fits-partition check (e.g. when validating ahead of partition
// lookup). Streams the file in CHUNK-sized reads; the file is rewound on
// success so the caller can immediately reread it for flashing.
Result validateImageFile(const char* sdPath, size_t partitionSize);

const char* resultName(Result r);

// Returns the chip_id (esp_image_header_t offset 12) of the currently-running
// image, or 0xFFFF if it cannot be read. Because the running slot booted
// successfully, its chip_id is authoritative for the current CPU, so a
// candidate image must match it to be safe to flash.
uint16_t runningPartitionChipId();

}  // namespace firmware_flash
