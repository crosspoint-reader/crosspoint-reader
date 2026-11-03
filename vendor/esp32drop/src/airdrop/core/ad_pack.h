/* ad_pack.h -- build the cpio archive an AirDrop SENDER has to deliver.
 *
 * The last piece of the /Upload body. The other two already exist and are measured:
 * ad_zlib.h emits a conforming zlib stream out of DEFLATE stored blocks (no compressor,
 * no allocation -- the ROM's tdefl wants 167,744 bytes of state and does not fit), and
 * adz_dvzip_record() wraps one in the [BE32 length][zlib] framing a receiver expects.
 * What was missing is the archive inside.
 *
 * THE FORMAT IS NOT GUESSED. Every field below is copied off a real Apple sender's own
 * bytes, from testdata/dvzip_body.bin -- a captured /Upload body:
 *
 *   magic    '070707'      dev  '000000'   ino '000002'   mode '100644'
 *   uid      '000765'      gid  '000024'   nlink '000001' rdev '000000'
 *   mtime    '15245051022' namesize '000021' filesize '00000001135'
 *   name     './._IMG_7495.JPG\0'
 *
 *   ...and the archive ends with, exactly:
 *   magic '070707' dev '000000' ino '000003' mode '000000' uid '000000' gid '000000'
 *   nlink '000001' rdev '000000' mtime '00000000000' namesize '000013'
 *   filesize '00000000000'   name 'TRAILER!!!\0'
 *
 * ⚠️ uid 0765 and gid 0024 are 501 and 20 -- the SENDING HUMAN's real uid and gid. Those
 * are not ours to copy: a badge has no user. This writes 0/0, which a receiver reads as
 * root and does not care about, and which leaks nothing about anybody.
 *
 * WHAT A TRANSFER LOOKS LIKE. Measured across all thirteen archives in the capture
 * corpus, every one had exactly two entries: a '.' directory and one './name' file
 * (ad_cpio.h's own note). So that is the shape emitted here.
 * ⚠️ The '.' entry's permission bits are [A]: only its TYPE bits were ever observed to
 * matter (ad_cpio.h filters on AD_CPIO_S_IFDIR), and no capture in this tree shows the
 * directory header itself. 040755 is the conventional value.
 *
 * Dependency-free integer C, in the house style: the same code compiles into the firmware
 * and into tools/test-pack.sh, where the output is checked by macOS's own cpio(1) --
 * an implementation that is not this one, which is the only kind of golden worth having.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AD_PACK_HDR 76u            /* the odc header, exactly: 6+6+6+6+6+6+6+6+11+6+11  */
#define AD_PACK_DIR_MODE 040755u   /* [A] -- see the header note                        */
#define AD_PACK_FILE_MODE 0100644u /* [M] -- '100644' off the wire                      */

/* mtime is the caller's to supply. A badge has no clock it trusts on its own, and writing
   zero makes every received file land in the receiver's Downloads dated 1 Jan 1970 -- which
   is not a cosmetic detail when the folder is sorted by date. Seconds since the Unix epoch;
   0 keeps the old behaviour for a caller that genuinely has no time source. */
struct AdPackFile {
  const char* name; /* basename; './' is prepended, as real senders do */
  const uint8_t* data;
  uint32_t len;
  uint32_t mtime; /* seconds since the Unix epoch; 0 = unknown */
};

/* n ASCII octal digits, zero-padded, no NUL. Refuses a value that will not fit rather
   than truncating: a truncated length field silently reinterprets the payload. */
static bool ad_pack_oct(uint8_t* p, int n, uint32_t v) {
  for (int i = n - 1; i >= 0; i--) {
    p[i] = (uint8_t)('0' + (v & 7));
    v >>= 3;
  }
  return v == 0;
}

/* One odc header. `ino` is sequential, as the real sender's was (000002 then 000003). */
static bool ad_pack_hdr(uint8_t* p, uint32_t ino, uint32_t mode, uint32_t namesize, uint32_t filesize, uint32_t mtime) {
  memcpy(p, "070707", 6);
  bool ok = true;
  ok &= ad_pack_oct(p + 6, 6, 0); /* dev                                    */
  ok &= ad_pack_oct(p + 12, 6, ino);
  ok &= ad_pack_oct(p + 18, 6, mode);
  ok &= ad_pack_oct(p + 24, 6, 0);      /* uid  -- not the sender's; see the note  */
  ok &= ad_pack_oct(p + 30, 6, 0);      /* gid                                    */
  ok &= ad_pack_oct(p + 36, 6, 1);      /* nlink                                  */
  ok &= ad_pack_oct(p + 42, 6, 0);      /* rdev                                   */
  ok &= ad_pack_oct(p + 48, 11, mtime); /* 0 if the caller has no clock */
  ok &= ad_pack_oct(p + 59, 6, namesize);
  ok &= ad_pack_oct(p + 65, 11, filesize);
  return ok;
}

/* Exact size of the archive ad_pack_cpio() will write. Exact, not an upper bound: the
   caller has to size a PSRAM block and a DvZip record around it. */
static uint32_t ad_pack_bound(const struct AdPackFile* f, int n) {
  if (n < 0) return 0;
  uint32_t t = AD_PACK_HDR + 2u; /* the '.' entry: name ".\0"     */
  for (int i = 0; i < n; i++) {
    if (!f[i].name) return 0;
    uint32_t nl = (uint32_t)strlen(f[i].name);
    t += AD_PACK_HDR + 2u + nl + 1u + f[i].len; /* "./" + name + NUL + payload   */
  }
  t += AD_PACK_HDR + 11u; /* "TRAILER!!!\0"                */
  return t;
}

/* Write the archive. Returns bytes written, or 0 on any refusal -- no room, a name or
 * length that will not fit its octal field, a null pointer. Never a partial archive: a
 * short cpio is not a smaller archive, it is one that stops mid-entry, and a receiver
 * reads whatever follows as the next header.
 *
 * There is no padding between entries. odc is unaligned by definition -- that is the
 * difference from the 'newc' format, and ad_cpio.h's reader depends on it.
 */
static uint32_t ad_pack_cpio(uint8_t* out, uint32_t cap, const struct AdPackFile* f, int n) {
  if (!out || n < 0 || (n > 0 && !f)) return 0;
  uint32_t need = ad_pack_bound(f, n);
  if (need == 0 || need > cap) return 0;

  uint8_t* p = out;
  uint32_t ino = 2; /* the real sender started at 2  */

  /* The '.' directory entry, filesize 0. Every captured archive had one. */
  if (!ad_pack_hdr(p, ino++, AD_PACK_DIR_MODE, 2, 0, 0)) return 0;
  p += AD_PACK_HDR;
  *p++ = '.';
  *p++ = 0;

  for (int i = 0; i < n; i++) {
    uint32_t nl = (uint32_t)strlen(f[i].name);
    if (nl == 0) return 0;
    if (f[i].len && !f[i].data) return 0;
    uint32_t namesize = 2u + nl + 1u; /* "./" + name + NUL             */
    if (!ad_pack_hdr(p, ino++, AD_PACK_FILE_MODE, namesize, f[i].len, f[i].mtime)) return 0;
    p += AD_PACK_HDR;
    *p++ = '.';
    *p++ = '/';
    memcpy(p, f[i].name, nl);
    p += nl;
    *p++ = 0;
    if (f[i].len) {
      memcpy(p, f[i].data, f[i].len);
      p += f[i].len;
    }
  }

  if (!ad_pack_hdr(p, ino, 0, 11, 0, 0)) return 0;
  p += AD_PACK_HDR;
  memcpy(p, "TRAILER!!!", 10);
  p += 10;
  *p++ = 0;

  return (uint32_t)(p - out);
}
