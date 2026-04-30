"""
PlatformIO post-build script: patch firmware.bin for X3-era bootloader compatibility.

The X3 ships with an older ESP-IDF bootloader that misreads two newer
esp_app_desc_t fields and rejects the image with messages like:

    E (xxxxx) boot_comm: Image requires efuse blk rev >= v625.94, but chip is v1.3
    E (xxxxx) boot_comm: Image requires efuse blk rev <= v0.16, but chip is v1.3

Fix: rewrite min/max_efuse_blk_rev_full inside esp_app_desc_t (which lives at
the start of the first segment's data) to neutral values (0 / 0xFFFF), then
recompute both the segment checksum (last byte before SHA) and the SHA256
trailer so the bootloader's integrity checks still pass.

Image layout we rely on:

    [ esp_image_header_t (24B) ]              <- offset 0
    [ segment_0 header (8B)    ]              <- offset 24
    [ segment_0 data (esp_app_desc_t...) ]    <- offset 32 (esp_app_desc_t at +32..+287)
    [ segment_1 header + data  ]
    ...
    [ pad bytes ]                             <- so total len before checksum is aligned
    [ checksum byte ]                         <- last byte of 16-byte-aligned region
    [ SHA256 (32B) ]                          <- only present when hash_appended == 1

Idempotent — patched images are detected by the already-cleared min_efuse field.
"""

Import("env")
import hashlib
import os
import struct

ESP_IMAGE_MAGIC = 0xE9
APP_DESC_MAGIC = 0xABCD5432
CHECKSUM_SEED = 0xEF
SHA_TRAILER = 32

OFF_HASH_APPENDED = 23

APP_DESC_OFFSET = 24 + 8
# esp_app_desc_t layout (ESP-IDF v5.x):
#   magic(4)+secure(4)+reserv1[2](8)+version[32]+project_name[32]
#   +time[16]+date[16]+idf_ver[32]+app_elf_sha256[32] = 176 bytes,
# so min_efuse_blk_rev_full sits at +176, max at +178.
OFF_MIN_EFUSE = APP_DESC_OFFSET + 176
OFF_MAX_EFUSE = APP_DESC_OFFSET + 178


def _recompute_segment_checksum(data, hash_appended):
    """Walk the segment table, XOR every data byte, store result at the
    16-byte-aligned checksum slot. Returns the new full image bytes."""
    seg_count = data[1]
    pos = 24
    checksum = CHECKSUM_SEED
    for i in range(seg_count):
        if pos + 8 > len(data):
            raise RuntimeError(f"segment {i} header out of range at {pos}")
        _, data_len = struct.unpack_from("<II", data, pos)
        pos += 8
        seg_end = pos + data_len
        if seg_end > len(data):
            raise RuntimeError(f"segment {i} data out of range ({pos}..{seg_end} > {len(data)})")
        for b in data[pos:seg_end]:
            checksum ^= b
        pos = seg_end

    # The checksum byte sits at the last byte of a 16-byte-aligned padded region.
    pad_end = (pos + 16) & ~15
    checksum_pos = pad_end - 1

    body_len = pad_end
    sha_len = SHA_TRAILER if hash_appended else 0
    expected_total = body_len + sha_len
    if expected_total != len(data):
        raise RuntimeError(
            f"image size mismatch: expected {expected_total} (body {body_len} + sha {sha_len}), "
            f"have {len(data)}"
        )

    out = bytearray(data)
    for i in range(pos, checksum_pos):
        out[i] = 0
    out[checksum_pos] = checksum & 0xFF

    if hash_appended:
        body = bytes(out[:body_len])
        sha = hashlib.sha256(body).digest()
        out[body_len:body_len + SHA_TRAILER] = sha

    return bytes(out)


def patch_firmware_bin(target, source, env):
    bin_path = str(target[0])
    if not bin_path.endswith(".bin"):
        return
    if not os.path.isfile(bin_path):
        print(f"[X3-PATCH] firmware not found: {bin_path}")
        return

    with open(bin_path, "rb") as f:
        data = bytearray(f.read())

    if len(data) < OFF_MAX_EFUSE + 2:
        print(f"[X3-PATCH] firmware too small to patch ({len(data)} bytes)")
        return
    if data[0] != ESP_IMAGE_MAGIC:
        print(f"[X3-PATCH] not an ESP32 image (magic=0x{data[0]:02X}); skipping")
        return

    app_desc_magic = struct.unpack_from("<I", data, APP_DESC_OFFSET)[0]
    if app_desc_magic != APP_DESC_MAGIC:
        print(f"[X3-PATCH] unexpected app desc magic 0x{app_desc_magic:08X}; aborting")
        return

    orig_min_efuse = struct.unpack_from("<H", data, OFF_MIN_EFUSE)[0]
    orig_max_efuse = struct.unpack_from("<H", data, OFF_MAX_EFUSE)[0]
    if orig_min_efuse == 0 and orig_max_efuse == 0xFFFF:
        print("[X3-PATCH] firmware already patched; skipping")
        return

    struct.pack_into("<H", data, OFF_MIN_EFUSE, 0x0000)
    struct.pack_into("<H", data, OFF_MAX_EFUSE, 0xFFFF)

    hash_appended = bool(data[OFF_HASH_APPENDED])

    patched = _recompute_segment_checksum(bytes(data), hash_appended)

    with open(bin_path, "wb") as f:
        f.write(patched)

    # Self-verify the produced image (CI-style sanity check).
    _verify_image(patched, hash_appended)

    print(
        "[X3-PATCH] patched {0}: min_efuse_blk_rev_full {1}->0, max_efuse_blk_rev_full {2}->65535, "
        "checksum+SHA recomputed (hash_appended={3})".format(
            os.path.basename(bin_path), orig_min_efuse, orig_max_efuse, int(hash_appended)
        )
    )


def _verify_image(data, hash_appended):
    """Re-walk the image and confirm checksum + SHA match. Raises on mismatch."""
    seg_count = data[1]
    pos = 24
    checksum = CHECKSUM_SEED
    for _ in range(seg_count):
        _, data_len = struct.unpack_from("<II", data, pos)
        pos += 8
        for b in data[pos:pos + data_len]:
            checksum ^= b
        pos += data_len
    pad_end = (pos + 16) & ~15
    expected = checksum & 0xFF
    actual = data[pad_end - 1]
    if expected != actual:
        raise RuntimeError(f"[X3-PATCH] self-check FAILED: checksum 0x{actual:02X} != 0x{expected:02X}")
    if hash_appended:
        body = data[:pad_end]
        sha = hashlib.sha256(body).digest()
        if sha != data[pad_end:pad_end + SHA_TRAILER]:
            raise RuntimeError("[X3-PATCH] self-check FAILED: SHA256 mismatch")
    print(f"[X3-PATCH] self-check OK: checksum=0x{actual:02X} hash_appended={int(hash_appended)}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", patch_firmware_bin)
