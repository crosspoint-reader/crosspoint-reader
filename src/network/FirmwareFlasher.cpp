#include "FirmwareFlasher.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_app_format.h>
#include <esp_chip_info.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <sdkconfig.h>
#include <spi_flash_mmap.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "FirmwareBoardTag.h"
#include "FirmwareImageIdentity.h"
#include "OtaBootSwitch.h"

namespace firmware_flash {

namespace {
constexpr uint8_t ESP_IMAGE_MAGIC = 0xE9;
constexpr size_t MIN_FIRMWARE_SIZE = 64 * 1024;
constexpr size_t SEC = SPI_FLASH_SEC_SIZE;  // 4 KiB
constexpr size_t BLK = 64 * 1024;           // 64 KiB block-erase granularity
constexpr size_t CHUNK = 4096;
constexpr size_t SHA_TRAILER = 32;
constexpr uint8_t CHECKSUM_SEED = 0xEF;
constexpr size_t HEADER_SIZE = firmware_identity::IMAGE_HEADER_SIZE;
constexpr size_t SEG_HEADER_SIZE = 8;

// The pure seam mirrors esp_chip_id_t / esp_chip_model_t so the host tests can
// run without ESP-IDF. Pin both tables to the real enums here, where the SDK
// headers are in scope, so a future SDK renumbering breaks the build instead of
// silently mislabelling a chip.
static_assert(firmware_identity::CHIP_ID_UNKNOWN == ESP_CHIP_ID_INVALID, "chip id sentinel drift");
static_assert(firmware_identity::CHIP_ID_ESP32 == ESP_CHIP_ID_ESP32, "chip id drift");
static_assert(firmware_identity::CHIP_ID_ESP32S2 == ESP_CHIP_ID_ESP32S2, "chip id drift");
static_assert(firmware_identity::CHIP_ID_ESP32S3 == ESP_CHIP_ID_ESP32S3, "chip id drift");
static_assert(firmware_identity::CHIP_ID_ESP32C3 == ESP_CHIP_ID_ESP32C3, "chip id drift");
static_assert(firmware_identity::CHIP_ID_ESP32C2 == ESP_CHIP_ID_ESP32C2, "chip id drift");
static_assert(firmware_identity::CHIP_ID_ESP32C6 == ESP_CHIP_ID_ESP32C6, "chip id drift");
static_assert(firmware_identity::CHIP_ID_ESP32H2 == ESP_CHIP_ID_ESP32H2, "chip id drift");
static_assert(firmware_identity::CHIP_ID_ESP32P4 == ESP_CHIP_ID_ESP32P4, "chip id drift");
static_assert(firmware_identity::CHIP_ID_ESP32C61 == ESP_CHIP_ID_ESP32C61, "chip id drift");
static_assert(firmware_identity::CHIP_ID_ESP32C5 == ESP_CHIP_ID_ESP32C5, "chip id drift");
static_assert(firmware_identity::CHIP_ID_ESP32H21 == ESP_CHIP_ID_ESP32H21, "chip id drift");
static_assert(firmware_identity::CHIP_ID_ESP32H4 == ESP_CHIP_ID_ESP32H4, "chip id drift");
static_assert(firmware_identity::MODEL_ESP32 == CHIP_ESP32, "chip model drift");
static_assert(firmware_identity::MODEL_ESP32S2 == CHIP_ESP32S2, "chip model drift");
static_assert(firmware_identity::MODEL_ESP32S3 == CHIP_ESP32S3, "chip model drift");
static_assert(firmware_identity::MODEL_ESP32C3 == CHIP_ESP32C3, "chip model drift");
static_assert(firmware_identity::MODEL_ESP32C2 == CHIP_ESP32C2, "chip model drift");
static_assert(firmware_identity::MODEL_ESP32C6 == CHIP_ESP32C6, "chip model drift");
static_assert(firmware_identity::MODEL_ESP32H2 == CHIP_ESP32H2, "chip model drift");
static_assert(firmware_identity::MODEL_ESP32P4 == CHIP_ESP32P4, "chip model drift");
static_assert(firmware_identity::MODEL_ESP32C61 == CHIP_ESP32C61, "chip model drift");
static_assert(firmware_identity::MODEL_ESP32C5 == CHIP_ESP32C5, "chip model drift");
static_assert(firmware_identity::MODEL_ESP32H21 == CHIP_ESP32H21, "chip model drift");
static_assert(firmware_identity::MODEL_ESP32H4 == CHIP_ESP32H4, "chip model drift");
}  // namespace

const char* resultName(Result r) {
  switch (r) {
    case Result::OK:
      return "OK";
    case Result::OPEN_FAIL:
      return "OPEN_FAIL";
    case Result::TOO_SMALL:
      return "TOO_SMALL";
    case Result::TOO_LARGE:
      return "TOO_LARGE";
    case Result::BAD_MAGIC:
      return "BAD_MAGIC";
    case Result::BAD_SEGMENTS:
      return "BAD_SEGMENTS";
    case Result::BAD_CHECKSUM:
      return "BAD_CHECKSUM";
    case Result::BAD_SHA:
      return "BAD_SHA";
    case Result::BAD_CHIP:
      return "BAD_CHIP";
    case Result::WRONG_BOARD:
      return "WRONG_BOARD";
    case Result::BAD_SIZE:
      return "BAD_SIZE";
    case Result::NO_PARTITION:
      return "NO_PARTITION";
    case Result::OOM:
      return "OOM";
    case Result::READ_FAIL:
      return "READ_FAIL";
    case Result::ERASE_FAIL:
      return "ERASE_FAIL";
    case Result::WRITE_FAIL:
      return "WRITE_FAIL";
    case Result::OTADATA_FAIL:
      return "OTADATA_FAIL";
  }
  return "?";
}

uint16_t deviceChipId() {
#ifdef CONFIG_IDF_FIRMWARE_CHIP_ID
  // The build target's own chip id, resolved at compile time. This is the exact
  // constant bootloader_common_check_chip_id() compares every image's header
  // against on every boot, so the guard below predicts the bootloader's verdict
  // instead of re-deriving device identity from storage.
  static constexpr uint16_t BUILD_TARGET_CHIP_ID = static_cast<uint16_t>(CONFIG_IDF_FIRMWARE_CHIP_ID);
  static_assert(BUILD_TARGET_CHIP_ID != firmware_identity::CHIP_ID_UNKNOWN,
                "CONFIG_IDF_FIRMWARE_CHIP_ID is the invalid-chip sentinel");
  return BUILD_TARGET_CHIP_ID;
#else
  // No build-target constant: ask the silicon. esp_chip_info() reports an
  // esp_chip_model_t, a different enum from esp_chip_id_t, so it goes through
  // the explicit mapping -- a cast already mislabels ESP32 (model 1, id
  // 0x0000) and nothing guarantees the rest keep agreeing.
  esp_chip_info_t info = {};
  esp_chip_info(&info);
  const uint16_t id = firmware_identity::chipIdForModel(static_cast<int>(info.model));
  if (id == firmware_identity::CHIP_ID_UNKNOWN) {
    LOG_ERR("FLASH", "unmapped chip model %d; chip guard disabled", static_cast<int>(info.model));
  }
  return id;
#endif
}

namespace {
// Stream `length` bytes from `file` starting at the current read offset, feeding them through
// both the XOR-checksum and SHA256 accumulators. Used by validateImageFile so the whole image
// is verified end-to-end without holding it in RAM (ESP32-C3 only has ~380 KB).
Result feedHashAndChecksum(HalFile& file, size_t length, uint8_t* xorAccum, mbedtls_sha256_context* sha, uint8_t* buf,
                           board_tag::Scanner* tagScanner) {
  size_t remaining = length;
  while (remaining > 0) {
    const size_t want = std::min<size_t>(CHUNK, remaining);
    const int got = file.read(buf, want);
    if (got <= 0 || static_cast<size_t>(got) != want) return Result::READ_FAIL;
    if (sha) mbedtls_sha256_update(sha, buf, want);
    if (tagScanner) tagScanner->feed(buf, want);
    if (xorAccum) {
      uint8_t acc = *xorAccum;
      for (size_t i = 0; i < want; i++) acc ^= buf[i];
      *xorAccum = acc;
    }
    remaining -= want;
  }
  return Result::OK;
}
}  // namespace

Result validateImageFile(const char* sdPath, size_t partitionSize) {
  HalFile file;
  if (!Storage.openFileForRead("FLASH", sdPath, file) || !file) {
    LOG_ERR("FLASH", "validate: open failed: %s", sdPath);
    return Result::OPEN_FAIL;
  }

  const size_t fileSize = file.fileSize();
  if (fileSize < MIN_FIRMWARE_SIZE) {
    LOG_ERR("FLASH", "validate: too small: %u", static_cast<unsigned>(fileSize));
    file.close();
    return Result::TOO_SMALL;
  }
  if (partitionSize > 0 && fileSize > partitionSize) {
    LOG_ERR("FLASH", "validate: too large: %u > %u", static_cast<unsigned>(fileSize),
            static_cast<unsigned>(partitionSize));
    file.close();
    return Result::TOO_LARGE;
  }

  uint8_t header[HEADER_SIZE];
  if (file.read(header, HEADER_SIZE) != static_cast<int>(HEADER_SIZE)) {
    LOG_ERR("FLASH", "validate: header read failed");
    file.close();
    return Result::READ_FAIL;
  }
  if (header[0] != ESP_IMAGE_MAGIC) {
    LOG_ERR("FLASH", "validate: bad magic 0x%02X", header[0]);
    file.close();
    return Result::BAD_MAGIC;
  }
  // Reject an image built for a different MCU family before it can brick the
  // device. Both sides go through firmware_identity so this path and the OTA
  // path cannot drift apart.
  uint16_t imageChip = 0;
  if (!firmware_identity::readImageChipId(header, HEADER_SIZE, imageChip)) {
    // Unreachable while HEADER_SIZE covers the field, but an unread chip id is
    // reported as a read failure rather than defaulting to a value: 0x0000 is
    // ESP32's real chip id, so a zeroed local is not a usable "unknown".
    LOG_ERR("FLASH", "validate: chip id not readable from header");
    file.close();
    return Result::READ_FAIL;
  }
  const uint16_t deviceChip = deviceChipId();
  switch (firmware_identity::compareChipId(imageChip, deviceChip)) {
    case firmware_identity::ChipVerdict::Mismatch:
      LOG_ERR("FLASH", "validate: wrong chip: image=0x%04X device=0x%04X", imageChip, deviceChip);
      file.close();
      return Result::BAD_CHIP;
    case firmware_identity::ChipVerdict::UnknownDeviceIdentity:
      // Fail open: refusing here would leave a device whose identity source is
      // broken unable to install any image at all. The bootloader still checks
      // chip_id on boot, and the board-tag scan below is unaffected.
      LOG_ERR("FLASH", "validate: device chip id unknown; chip guard skipped (image=0x%04X)", imageChip);
      break;
    case firmware_identity::ChipVerdict::Match:
      break;
  }
  const uint8_t segCount = header[1];
  const bool hashAppended = header[23] != 0;

  auto buf = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[CHUNK]);
  if (!buf) {
    file.close();
    return Result::OOM;
  }

  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, /*is224=*/0);
  mbedtls_sha256_update(&shaCtx, header, HEADER_SIZE);

  uint8_t xorAccum = CHECKSUM_SEED;
  size_t pos = HEADER_SIZE;
  // Board tag: scanned from the same segment stream the hash pass already
  // reads, so the check is free of extra I/O. Only a present-and-mismatched
  // tag rejects; untagged images (forks, other projects) pass.
  board_tag::Scanner tagScanner;

  for (uint8_t i = 0; i < segCount; i++) {
    if (pos + SEG_HEADER_SIZE > fileSize) {
      LOG_ERR("FLASH", "validate: seg %u header overruns EOF at %u", i, static_cast<unsigned>(pos));
      mbedtls_sha256_free(&shaCtx);
      file.close();
      return Result::BAD_SEGMENTS;
    }
    uint8_t segHdr[SEG_HEADER_SIZE];
    if (file.read(segHdr, SEG_HEADER_SIZE) != static_cast<int>(SEG_HEADER_SIZE)) {
      mbedtls_sha256_free(&shaCtx);
      file.close();
      return Result::READ_FAIL;
    }
    mbedtls_sha256_update(&shaCtx, segHdr, SEG_HEADER_SIZE);
    pos += SEG_HEADER_SIZE;

    uint32_t dataLen;
    std::memcpy(&dataLen, segHdr + 4, sizeof(dataLen));
    if (pos + dataLen > fileSize) {
      LOG_ERR("FLASH", "validate: seg %u data overruns EOF (%u + %u > %u)", i, static_cast<unsigned>(pos),
              static_cast<unsigned>(dataLen), static_cast<unsigned>(fileSize));
      mbedtls_sha256_free(&shaCtx);
      file.close();
      return Result::BAD_SEGMENTS;
    }

    const Result feedRes = feedHashAndChecksum(file, dataLen, &xorAccum, &shaCtx, buf.get(), &tagScanner);
    if (feedRes != Result::OK) {
      mbedtls_sha256_free(&shaCtx);
      file.close();
      return feedRes;
    }
    pos += dataLen;
  }

  if (tagScanner.mismatch()) {
    LOG_ERR("FLASH", "validate: wrong board: image=%s device=%.*s", tagScanner.foundName(),
            static_cast<int>(board_tag::boardNameLen()), board_tag::boardName());
    mbedtls_sha256_free(&shaCtx);
    file.close();
    return Result::WRONG_BOARD;
  }

  // pad_end is the 16-byte aligned offset at which the checksum byte sits at pad_end - 1.
  const size_t padEnd = (pos + 16) & ~static_cast<size_t>(15);
  const size_t expectedTotal = padEnd + (hashAppended ? SHA_TRAILER : 0);
  if (expectedTotal != fileSize) {
    LOG_ERR("FLASH", "validate: size mismatch body+pad=%u sha=%u expected=%u actual=%u", static_cast<unsigned>(padEnd),
            static_cast<unsigned>(hashAppended ? SHA_TRAILER : 0), static_cast<unsigned>(expectedTotal),
            static_cast<unsigned>(fileSize));
    mbedtls_sha256_free(&shaCtx);
    file.close();
    return Result::BAD_SIZE;
  }

  // Read the padding bytes (which include the stored checksum at the last byte) into the SHA stream.
  const size_t padLen = padEnd - pos;
  uint8_t padBuf[16];
  if (padLen > sizeof(padBuf)) {
    mbedtls_sha256_free(&shaCtx);
    file.close();
    return Result::BAD_SIZE;
  }
  if (padLen > 0 && file.read(padBuf, padLen) != static_cast<int>(padLen)) {
    mbedtls_sha256_free(&shaCtx);
    file.close();
    return Result::READ_FAIL;
  }
  mbedtls_sha256_update(&shaCtx, padBuf, padLen);

  const uint8_t storedChecksum = padBuf[padLen - 1];
  if ((xorAccum & 0xFF) != storedChecksum) {
    LOG_ERR("FLASH", "validate: checksum mismatch computed=0x%02X stored=0x%02X", xorAccum, storedChecksum);
    mbedtls_sha256_free(&shaCtx);
    file.close();
    return Result::BAD_CHECKSUM;
  }

  if (hashAppended) {
    uint8_t computed[SHA_TRAILER];
    mbedtls_sha256_finish(&shaCtx, computed);
    uint8_t stored[SHA_TRAILER];
    if (file.read(stored, SHA_TRAILER) != static_cast<int>(SHA_TRAILER)) {
      mbedtls_sha256_free(&shaCtx);
      file.close();
      return Result::READ_FAIL;
    }
    if (std::memcmp(computed, stored, SHA_TRAILER) != 0) {
      LOG_ERR("FLASH", "validate: SHA256 mismatch");
      mbedtls_sha256_free(&shaCtx);
      file.close();
      return Result::BAD_SHA;
    }
  }

  mbedtls_sha256_free(&shaCtx);
  file.close();
  return Result::OK;
}

Result flashFromSdPath(const char* sdPath, ProgressCb onProgress, void* ctx, bool alreadyValidated) {
  // Resolve destination first so we can size-check during validation. The full image-integrity
  // pass below verifies header, segment table, XOR checksum and SHA256 trailer end-to-end before
  // we touch otadata, so a truncated/corrupted .bin can never become the next boot target.
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    LOG_ERR("FLASH", "no next-update partition");
    return Result::NO_PARTITION;
  }

  // When the caller already ran validateImageFile() against this same partition
  // size (e.g. SdFirmwareUpdateActivity validates before the confirmation
  // prompt), skip the redundant integrity scan. We still keep the partition
  // lookup so the rest of the flashing path stays unchanged.
  if (!alreadyValidated) {
    const Result validateRes = validateImageFile(sdPath, dest->size);
    if (validateRes != Result::OK) {
      LOG_ERR("FLASH", "image validation failed: %s", resultName(validateRes));
      return validateRes;
    }
  }

  HalFile file;
  if (!Storage.openFileForRead("FLASH", sdPath, file) || !file) {
    LOG_ERR("FLASH", "open failed: %s", sdPath);
    return Result::OPEN_FAIL;
  }

  const size_t firmwareSize = file.fileSize();
  LOG_INF("FLASH", "src=%s size=%u dest=%s @0x%x partsize=%u", sdPath, static_cast<unsigned>(firmwareSize), dest->label,
          static_cast<unsigned>(dest->address), static_cast<unsigned>(dest->size));

  auto buffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[CHUNK]);
  if (!buffer) {
    LOG_ERR("FLASH", "OOM");
    file.close();
    return Result::OOM;
  }

  // Interleave erase + write so the progress bar advances 0→100% smoothly
  // rather than stalling for several seconds during a single up-front erase.
  size_t streamPos = 0;
  size_t erasedUpto = 0;
  while (streamPos < firmwareSize) {
    if (streamPos >= erasedUpto) {
      size_t eraseLen = std::min<size_t>(BLK, dest->size - streamPos);
      eraseLen = (eraseLen + SEC - 1) & ~(SEC - 1);
      eraseLen = std::min<size_t>(eraseLen, dest->size - streamPos);
      if (esp_partition_erase_range(dest, streamPos, eraseLen) != ESP_OK) {
        LOG_ERR("FLASH", "erase @%u (len=%u) failed", static_cast<unsigned>(streamPos),
                static_cast<unsigned>(eraseLen));
        file.close();
        return Result::ERASE_FAIL;
      }
      erasedUpto = streamPos + eraseLen;
    }

    const size_t want = std::min<size_t>(CHUNK, firmwareSize - streamPos);
    const int read = file.read(buffer.get(), want);
    if (read <= 0 || static_cast<size_t>(read) != want) {
      LOG_ERR("FLASH", "read @%u: got=%d want=%u", static_cast<unsigned>(streamPos), read, static_cast<unsigned>(want));
      file.close();
      return Result::READ_FAIL;
    }
    if (esp_partition_write(dest, streamPos, buffer.get(), want) != ESP_OK) {
      LOG_ERR("FLASH", "write @%u failed", static_cast<unsigned>(streamPos));
      file.close();
      return Result::WRITE_FAIL;
    }
    streamPos += want;
    if (onProgress) onProgress(streamPos, firmwareSize, ctx);
    delay(1);
  }
  file.close();

  if (!ota_boot::switchTo(dest)) {
    LOG_ERR("FLASH", "otadata switch failed");
    return Result::OTADATA_FAIL;
  }
  return Result::OK;
}

}  // namespace firmware_flash
