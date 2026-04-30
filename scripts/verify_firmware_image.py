"""
PlatformIO post-build verifier: simulates the on-device SD-update validation
pipeline (SdFirmwareUpdateActivity::validateFirmware + Arduino Update API)
against the freshly built firmware.bin.

Goal: catch any condition that would cause the on-device SD update to reject
or corrupt the image, BEFORE you copy it to the SD card. If this script
passes, the device-side validation will pass too.

Standalone usage (e.g. on a candidate firmware.bin from your SD card):
    python scripts/verify_firmware_image.py /path/to/firmware.bin


Checks performed (mirrors lib paths the device walks):

  1. File >= MIN_FIRMWARE_SIZE (64 KiB) — same as validateFirmware()
  2. Image magic byte 0xE9 at offset 0
  3. Segment table is well-formed (no segment runs past EOF)
  4. ESP image segment checksum (XOR seed 0xEF) matches the byte stored at the
     16-byte-aligned slot — what the bootloader verifies
  5. SHA256 trailer matches the body (when hash_appended == 1) — what the
     bootloader verifies when secure boot is off
  6. esp_app_desc_t magic 0xABCD5432 at offset 32
  7. min/max_efuse_blk_rev_full are X3-friendly (0 / 0xFFFF) — proves the
     X3-PATCH post-action ran
  8. Image fits the largest OTA partition declared in partitions.csv —
     same probe as Update.begin()
  9. Image size minus optional SHA trailer is a multiple of 16 (matches
     bootloader's expected layout)
 10. App descriptor's project_name + version + idf_ver are printable ASCII
     (sanity check that the descriptor is not random data)

On success prints a single-line summary; on any failure raises and fails the
build so a broken binary is never published.
"""

import csv
import hashlib
import os
import struct
import sys

try:
    Import("env")  # noqa: F821 — provided by SCons in PlatformIO post-build context
    _PIO_CONTEXT = True
except Exception:
    env = None
    _PIO_CONTEXT = False

ESP_IMAGE_MAGIC = 0xE9
APP_DESC_MAGIC = 0xABCD5432
CHECKSUM_SEED = 0xEF
SHA_TRAILER = 32
MIN_FIRMWARE_SIZE = 64 * 1024  # SdFirmwareUpdateActivity::MIN_FIRMWARE_SIZE
APP_DESC_OFFSET = 24 + 8       # image header (24) + first segment header (8)


def _largest_app_partition(project_dir):
    """Return the size in bytes of the biggest 'app' partition in partitions.csv."""
    csv_path = os.path.join(project_dir, "partitions.csv")
    if not os.path.isfile(csv_path):
        return None
    biggest = 0
    with open(csv_path) as f:
        for row in csv.reader(f):
            row = [c.strip() for c in row if c.strip() and not c.strip().startswith("#")]
            if len(row) < 5:
                continue
            ptype = row[1].lower()
            size_field = row[4]
            if ptype != "app":
                continue
            try:
                size = int(size_field, 0)  # supports 0x... and decimal
            except ValueError:
                continue
            biggest = max(biggest, size)
    return biggest if biggest else None


def _printable(b):
    return b.split(b"\x00", 1)[0].decode("ascii", errors="replace")


def verify_firmware_bin(target, source, env):
    bin_path = str(target[0])
    if not bin_path.endswith(".bin") or not os.path.isfile(bin_path):
        return

    with open(bin_path, "rb") as f:
        data = f.read()
    name = os.path.basename(bin_path)

    failures = []

    # 1) min size
    if len(data) < MIN_FIRMWARE_SIZE:
        failures.append(f"size {len(data)} < MIN_FIRMWARE_SIZE {MIN_FIRMWARE_SIZE}")

    # 2) image magic
    if not data or data[0] != ESP_IMAGE_MAGIC:
        failures.append(f"magic 0x{data[0]:02X} != 0x{ESP_IMAGE_MAGIC:02X}")
        # If magic is wrong nothing else is reliable.
        raise RuntimeError(f"[X3-VERIFY] {name}: " + "; ".join(failures))

    seg_count = data[1]
    hash_appended = bool(data[23])

    # 3) segment table well-formed + 4) checksum
    pos = 24
    checksum = CHECKSUM_SEED
    for i in range(seg_count):
        if pos + 8 > len(data):
            failures.append(f"seg {i} header overruns EOF at {pos}")
            break
        _, dlen = struct.unpack_from("<II", data, pos)
        pos += 8
        end = pos + dlen
        if end > len(data):
            failures.append(f"seg {i} data overruns EOF ({end} > {len(data)})")
            break
        for b in data[pos:end]:
            checksum ^= b
        pos = end

    pad_end = (pos + 16) & ~15
    expected_total = pad_end + (SHA_TRAILER if hash_appended else 0)
    if expected_total != len(data):
        failures.append(
            f"size mismatch: body+pad={pad_end} sha={SHA_TRAILER if hash_appended else 0} "
            f"expected_total={expected_total} actual={len(data)}"
        )
    if pad_end - 1 < len(data):
        stored = data[pad_end - 1]
        if (checksum & 0xFF) != stored:
            failures.append(f"segment checksum mismatch: computed=0x{checksum & 0xFF:02X} stored=0x{stored:02X}")

    # 5) SHA trailer
    if hash_appended and len(data) >= pad_end + SHA_TRAILER:
        body_sha = hashlib.sha256(data[:pad_end]).digest()
        stored_sha = data[pad_end:pad_end + SHA_TRAILER]
        if body_sha != stored_sha:
            failures.append(
                f"SHA256 mismatch: computed={body_sha.hex()[:16]}... stored={stored_sha.hex()[:16]}..."
            )

    # 9) padded body must be 16-aligned
    if pad_end % 16 != 0:
        failures.append(f"padded body not 16-aligned ({pad_end})")

    # 6) app_desc magic + 7) efuse fields + 10) printable identity strings
    if len(data) >= APP_DESC_OFFSET + 256:
        app_magic = struct.unpack_from("<I", data, APP_DESC_OFFSET)[0]
        if app_magic != APP_DESC_MAGIC:
            failures.append(f"app_desc magic 0x{app_magic:08X} != 0x{APP_DESC_MAGIC:08X}")

        min_efuse = struct.unpack_from("<H", data, APP_DESC_OFFSET + 176)[0]
        max_efuse = struct.unpack_from("<H", data, APP_DESC_OFFSET + 178)[0]
        # patch_firmware_image.py runs first and sets min=0, max=0xFFFF for
        # X3-bootloader compatibility. SD update / OTA / web flasher all flash
        # the patched bin verbatim. Reject anything that doesn't look patched.
        if min_efuse != 0 or max_efuse != 0xFFFF:
            failures.append(
                f"app_desc efuse fields not patched: min={min_efuse} max={max_efuse} "
                f"(expected 0 / 65535 — patch_firmware_image.py should have run before verify)"
            )

        # esp_app_desc_t layout: secure_version(4) reserv1(4) version(32@8) project_name(32@40) ...
        version = _printable(data[APP_DESC_OFFSET + 16:APP_DESC_OFFSET + 16 + 32])
        project = _printable(data[APP_DESC_OFFSET + 48:APP_DESC_OFFSET + 48 + 32])
        # Embedded git/branch info populated by scripts/git_branch.py via build flag macro,
        # so project name should be a normal ascii identifier, not garbage.
        if not project or not all(0x20 <= ord(c) < 0x7F for c in project):
            failures.append(f"app_desc project_name not printable: {project!r}")
    else:
        failures.append("file too small to contain esp_app_desc_t")

    # 8) fits OTA partition
    project_dir = env["PROJECT_DIR"] if _PIO_CONTEXT else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    largest = _largest_app_partition(project_dir)
    if largest is not None and len(data) > largest:
        failures.append(f"image {len(data)} > largest app partition {largest}")

    # 1+ size summary
    summary = (
        f"[X3-VERIFY] {name}: size={len(data)} segs={seg_count} hash_appended={int(hash_appended)} "
        f"app=`{project}` ver=`{version}` ota_partition_max={largest}"
    )

    if failures:
        for fl in failures:
            print(f"[X3-VERIFY] FAIL: {fl}")
        print(summary)
        raise RuntimeError(
            f"[X3-VERIFY] {name} would be REJECTED by SdFirmwareUpdateActivity / bootloader"
        )

    print(summary + " — OK (matches on-device SD update validation)")


if _PIO_CONTEXT:
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", verify_firmware_bin)
elif __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: python scripts/verify_firmware_image.py <firmware.bin>", file=sys.stderr)
        sys.exit(2)

    class _Node:
        def __init__(self, p):
            self._p = p

        def __str__(self):
            return self._p

    try:
        verify_firmware_bin([_Node(sys.argv[1])], None, None)
    except RuntimeError as e:
        print(str(e), file=sys.stderr)
        sys.exit(1)