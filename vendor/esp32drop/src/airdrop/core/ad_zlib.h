/* ad_zlib.h -- emit a valid zlib stream with NO compressor.
 *
 * The AirDrop SEND path has to produce DvZip records: [BE32 length][zlib chunk].
 * The receive path already gets DEflate for free from the ESP32-S3 mask ROM
 * (tinfl_decompress_mem_to_mem, used by the receive path). The send path needs the
 * opposite direction, and the obvious answers are both bad:
 *
 *   - The ROM also exports tdefl_* (verified: esp32s3.rom.ld "Group miniz",
 *     tdefl_compress_mem_to_mem = 0x400007d4). But sizeof(tdefl_compressor) is
 *     167,744 bytes and the receiver runs with 29.7-37.6 KB of free internal heap
 *     (measured, logs/soakB_d1.txt). It does not fit.
 *   - Vendoring a deflate implementation is against org policy and would be ~1500
 *     lines to compress data that does not compress.
 *
 * Neither is needed. DEFLATE has a STORED block type (BTYPE=00) that copies bytes
 * through verbatim, so a conforming zlib stream can be emitted with a header, a
 * length, and a checksum -- no dictionary, no hash table, no allocation.
 *
 * MEASURED, not asserted (see tools/test-zlib.sh):
 * round-tripped byte-exact through real zlib -- both zlib.decompress and the
 * streaming decompressobj path a receiver uses -- at n = 0, 1, 100, 65534, 65535,
 * 65536, 131070, 200000.
 *
 * Is giving up compression a real cost? No, for this payload. A real Apple sender's
 * own deflate saved 1,719 bytes out of 266,752 on a captured PNG transfer = 0.64%,
 * because the payload was already compressed. Stored blocks cost +0.012% on the same
 * data. At the measured 5.7-26 KB/s link that difference is +0.07 to +0.3 seconds
 * against an effective RTT of ~1 second.
 *
 * Dependency-free integer C, in the house style: it compiles unchanged into the
 * firmware and into tools/test-zlib.sh.
 */
#pragma once
#include <stdint.h>
#include <string.h>

/* One stored block can carry at most 65535 bytes (LEN is a u16). */
#define ADZ_BLOCK_MAX 65535u

/* Adler-32 over the UNCOMPRESSED data. Kept separate so a streaming caller can
   accumulate it across chunks without holding the whole input. */
static uint32_t adz_adler32(uint32_t adler, const uint8_t* d, uint32_t n) {
  uint32_t a = adler & 0xffff, b = (adler >> 16) & 0xffff;
  for (uint32_t i = 0; i < n; i++) {
    a += d[i];
    if (a >= 65521) a -= 65521;
    b += a;
    if (b >= 65521) b -= 65521;
  }
  return (b << 16) | a;
}
#define ADZ_ADLER_INIT 1u

/* Exact output size for `n` input bytes. A caller MUST size its buffer with this:
   the emitter refuses rather than truncating, and a truncated zlib stream that still
   inflates as a prefix is precisely the failure mode awdl_http.h's DvZip completion
   probe exists to catch (a corrupt image reported as a successful transfer). */
static uint32_t adz_bound(uint32_t n) {
  uint32_t blocks = (n + ADZ_BLOCK_MAX - 1) / ADZ_BLOCK_MAX;
  if (blocks == 0) blocks = 1;      /* n == 0 still needs a final block */
  return 2u + blocks * 5u + n + 4u; /* hdr + per-block hdrs + data + adler */
}

/* Emit the complete stream. Returns bytes written, or 0 if `cap` is too small.
 *
 * Layout:
 *   78 01                        CMF=0x78 (deflate, 32K window), FLG=0x01.
 *                                (0x7801 % 31 == 0, which is the check zlib makes.)
 *   per block:
 *     [BFINAL | BTYPE=00]        one byte; stored blocks are byte-aligned, so the
 *                                remaining bits are the pad and must be zero
 *     [LEN lo][LEN hi]           little-endian
 *     [NLEN lo][NLEN hi]         = ~LEN, little-endian -- zlib verifies this
 *     [LEN raw bytes]
 *   [adler32]                    BIG-endian, over the uncompressed data
 */
static uint32_t adz_deflate_stored(uint8_t* out, uint32_t cap, const uint8_t* in, uint32_t n) {
  if (cap < adz_bound(n)) return 0;
  uint32_t p = 0;
  out[p++] = 0x78;
  out[p++] = 0x01;
  uint32_t off = 0;
  do {
    uint32_t len = (n - off > ADZ_BLOCK_MAX) ? ADZ_BLOCK_MAX : (n - off);
    out[p++] = (off + len >= n) ? 1 : 0; /* BFINAL in bit 0 */
    out[p++] = (uint8_t)(len & 0xff);
    out[p++] = (uint8_t)((len >> 8) & 0xff);
    uint16_t nlen = (uint16_t)~(uint16_t)len;
    out[p++] = (uint8_t)(nlen & 0xff);
    out[p++] = (uint8_t)((nlen >> 8) & 0xff);
    if (len) {
      memcpy(out + p, in + off, len);
      p += len;
    }
    off += len;
  } while (off < n);
  uint32_t ad = adz_adler32(ADZ_ADLER_INIT, in, n);
  out[p++] = (uint8_t)(ad >> 24);
  out[p++] = (uint8_t)(ad >> 16);
  out[p++] = (uint8_t)(ad >> 8);
  out[p++] = (uint8_t)ad;
  return p;
}

/* ------------------------------------------------------------------------- */
/* DvZip framing: a run of [BE32 chunk_len][zlib chunk], terminated by a
 * zero-length record.
 *
 * The reader half of this contract is dvzip_complete() in awdl_http.h, which was
 * written from captured Apple bytes; these writers are its mirror image and the
 * tests check them against each other. Note dvzip_complete treats a stream that ends
 * exactly on a record boundary as only a MAYBE (return 1) and requires corroborating
 * silence -- so a sender must emit the explicit end record, which is what makes the
 * receiver's answer certain (return 2) instead of timing-dependent.
 */

/* Size of one DvZip record wrapping `n` uncompressed bytes. */
static uint32_t adz_dvzip_record_bound(uint32_t n) { return 4u + adz_bound(n); }

/* Write one record. Returns bytes written, or 0 if `cap` is too small. */
static uint32_t adz_dvzip_record(uint8_t* out, uint32_t cap, const uint8_t* in, uint32_t n) {
  if (cap < adz_dvzip_record_bound(n)) return 0;
  uint32_t z = adz_deflate_stored(out + 4, cap - 4, in, n);
  if (z == 0) return 0;
  out[0] = (uint8_t)(z >> 24);
  out[1] = (uint8_t)(z >> 16);
  out[2] = (uint8_t)(z >> 8);
  out[3] = (uint8_t)z;
  return 4 + z;
}

/* Write the explicit end record. Four zero bytes; see the note above on why this is
   not optional. */
static uint32_t adz_dvzip_end(uint8_t* out, uint32_t cap) {
  if (cap < 4) return 0;
  out[0] = out[1] = out[2] = out[3] = 0;
  return 4;
}

/* Bound for a MULTI-record body: `n` bytes cut into records of at most `grain`
 * uncompressed bytes each, plus the end record. A real macOS sender frames its /Upload
 * this way (128 KiB grain, testdata/dvzip_census.inc); a receiver whose DvZip adapter
 * assumes that grain rejects a single oversized record, so a large send must be split. */
static uint32_t adz_dvzip_body_bound(uint32_t n, uint32_t grain) {
  if (grain == 0) grain = n ? n : 1;
  uint32_t total = 4u; /* the end record */
  uint32_t off = 0;
  do {
    uint32_t take = (n - off > grain) ? grain : (n - off);
    total += adz_dvzip_record_bound(take);
    off += take;
  } while (off < n);
  return total;
}

/* Write `n` bytes of `in` as a run of DvZip records, each carrying at most `grain`
 * uncompressed bytes, followed by the end record. Returns total bytes written, or 0 if
 * `cap` is too small. Mirror of adz_dvzip_body_bound(). */
static uint32_t adz_dvzip_body(uint8_t* out, uint32_t cap, const uint8_t* in, uint32_t n, uint32_t grain) {
  if (grain == 0) grain = n ? n : 1;
  uint32_t z = 0, off = 0;
  do {
    uint32_t take = (n - off > grain) ? grain : (n - off);
    uint32_t r = adz_dvzip_record(out + z, cap - z, in + off, take);
    if (r == 0) return 0;
    z += r;
    off += take;
  } while (off < n);
  uint32_t e = adz_dvzip_end(out + z, cap - z);
  if (e == 0) return 0;
  return z + e;
}
