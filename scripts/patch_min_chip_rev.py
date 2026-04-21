"""
Patch min_chip_rev_full to 0 in a firmware.bin and recompute the appended
SHA-256 digest so esp_image_verify() still passes.

esp_image_header_t layout (24 bytes, all packed):
  offset  0      magic (0xE9)
  offset  1      segment_count
  offset  2      spi_mode
  offset  3      spi_speed:4 | spi_size:4
  offset  4-7    entry_addr  (uint32 LE)
  offset  8      wp_pin
  offset  9-11   spi_pin_drv[3]
  offset  12-13  chip_id     (uint16 LE)
  offset  14     min_chip_rev (uint8, legacy)
  offset  15-16  min_chip_rev_full (uint16 LE)  <-- patch target
  offset  17-18  max_chip_rev_full (uint16 LE)
  offset  19-22  reserved[4]
  offset  23     hash_appended (uint8)

When hash_appended == 1, the last 32 bytes of the image are the SHA-256 over
bytes [0 .. len-33] (everything except the digest itself).  We recompute and
overwrite those 32 bytes after patching the header.
"""
import hashlib
import struct
import sys

MIN_CHIP_REV_FULL_OFFSET = 15  # byte offset of min_chip_rev_full in esp_image_header_t
HASH_LEN = 32
HASH_APPENDED_OFFSET = 23  # byte offset of hash_appended flag


def patch(path: str) -> None:
    with open(path, "r+b") as f:
        data = bytearray(f.read())

    if data[0] != 0xE9:
        print(f"ERROR: {path}: not a valid ESP image (magic=0x{data[0]:02x})", file=sys.stderr)
        sys.exit(1)

    old = struct.unpack_from("<H", data, MIN_CHIP_REV_FULL_OFFSET)[0]
    print(f"min_chip_rev_full: v{old // 100}.{old % 100} -> v0.0")
    struct.pack_into("<H", data, MIN_CHIP_REV_FULL_OFFSET, 0)

    hash_appended = data[HASH_APPENDED_OFFSET]
    if hash_appended == 1:
        # Digest covers everything except the last 32 bytes.
        digest = hashlib.sha256(data[:-HASH_LEN]).digest()
        data[-HASH_LEN:] = digest
        print(f"SHA-256 recomputed and written to last {HASH_LEN} bytes")
    else:
        print("hash_appended=0, no digest to update")

    with open(path, "wb") as f:
        f.write(data)

    print(f"Patched: {path}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <firmware.bin>", file=sys.stderr)
        sys.exit(1)
    patch(sys.argv[1])
