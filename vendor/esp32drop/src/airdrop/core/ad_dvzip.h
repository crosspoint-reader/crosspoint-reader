#ifndef AD_DVZIP_H
#define AD_DVZIP_H
/* DvZip record framing -- the outer structure of an AirDrop /Upload body.
 *
 * The body is a run of records, each a 4-byte big-endian header followed by that many
 * bytes of payload. Decompression is NOT here: it needs miniz, which is platform code,
 * and keeping the framing separate is what lets the framing be tested on a host against
 * captured Apple traffic. This header depends on nothing but <stdint.h>/<stdbool.h>.
 *
 * WHAT BIT 31 OF A RECORD HEADER MEANS -- settled by capture.
 *
 * It means the record is STORED: the payload is that many literal bytes, not a zlib stream.
 * The low 31 bits are the byte count either way.
 *
 * A macOS sender switches to stored records when deflate stops paying. Measured on a real
 * 2,352,508-byte body carrying a 2.35 MB photo (testdata/dvzip_census.inc is its shape with
 * the payloads removed; the body itself is somebody's photograph and is not in this
 * repository):
 *
 *     records  0..11   header 0x0001f6c9, 0x0002002e, ...   payload begins 78 9c
 *                      deflated, each inflating to exactly 131,072 bytes
 *     records 12..17   header 0x80020000 (and 0x8001ee00 last)
 *                      payload begins a2 a8, f1 5b, 61 9e, 7f d3, 4f 75, de 95 -- no zlib
 *                      header anywhere, high entropy, stored verbatim
 *     18 records consume 2,352,508 of 2,352,508 bytes and produce 2,354,688, which is a
 *     complete odc cpio ending in TRAILER!!!
 *
 * The record grain is 128 KiB of UNCOMPRESSED data throughout -- the last record is short
 * because the archive is. This also kills the two rival readings the byte counts could not
 * separate: the payloads carry no zlib header, so a flagged record is not still deflated;
 * and a 4-byte header walk lands exactly on the end across all 18 records, so there is no
 * extended 8-byte header.
 *
 * Before that capture this file returned the whole 32-bit word as a length, which made
 * every flagged record read as a 2 GiB demand and cut the stream. That is why a 2.4 MB
 * photo arrived as "archive truncated" while a small one worked: a small photo never gives
 * deflate enough to lose on.
 */

#include <stdbool.h>
#include <stdint.h>

#define AD_DVZIP_FLAG 0x80000000u /* bit 31, seen set on a real record header */

/* The record grain: every measured record inflates to exactly 128 KiB except the short
   last one. Not a protocol guarantee -- the sender picks it -- but a sound scale for
   heuristics, and the port's inflate-failure split leans on it by name. */
#define AD_DVZIP_GRAIN (128u * 1024)

/* How a walk stopped. Distinguishing these is the point: the shipping parser could not,
   so a stream that was cut short and one that ended cleanly reached the caller as the
   same thing -- a buffer and a length, reported as success. */
enum AdDvzipEnd {
  AD_DVZIP_END_RECORD = 0, /* an explicit zero-length record: structural certainty */
  AD_DVZIP_END_EXACT = 1,  /* ran out exactly on a record boundary: probably complete */
  AD_DVZIP_END_CUT = 2,    /* a record claims more than remains: incomplete or misread */
  AD_DVZIP_END_STRAND = 3, /* 1-3 bytes left over: too few for a header */
  AD_DVZIP_END_RUNNING = 4 /* the walk has not finished yet */
};

struct AdDvzipRec {
  uint32_t hdr; /* the 4 header bytes, big-endian, exactly as they arrived */
  uint32_t off; /* payload offset within the buffer */
  uint32_t len; /* payload byte count, low 31 bits of the header */
  bool stored;  /* true: copy the payload. false: inflate it. */
};

struct AdDvzipIter {
  const uint8_t* buf;
  uint32_t len, pos;
  uint32_t nrec;     /* records yielded so far */
  int end;           /* enum AdDvzipEnd */
  uint32_t cut_hdr;  /* on AD_DVZIP_END_CUT: the header bytes, verbatim */
  uint32_t cut_want; /* on AD_DVZIP_END_CUT: the length it was read as */
  uint32_t cut_have; /* on AD_DVZIP_END_CUT: what was left */
};

/* Payload length: the low 31 bits. Masking is a no-op on an unflagged header, so this is
   correct for both kinds of record. */
static inline uint32_t ad_dvzip_reclen(uint32_t hdr) { return hdr & ~AD_DVZIP_FLAG; }

/* True when bit 31 is set: the payload is stored verbatim rather than deflated. */
static inline bool ad_dvzip_is_flagged(uint32_t hdr) { return (hdr & AD_DVZIP_FLAG) != 0; }

static inline const char* ad_dvzip_end_name(int e) {
  switch (e) {
    case AD_DVZIP_END_RECORD:
      return "end-record";
    case AD_DVZIP_END_EXACT:
      return "exact";
    case AD_DVZIP_END_CUT:
      return "cut";
    case AD_DVZIP_END_STRAND:
      return "strand";
    default:
      return "running";
  }
}

/* True when a walk finished on a boundary rather than in the middle of a record. Only
   AD_DVZIP_END_RECORD is certainty; AD_DVZIP_END_EXACT is a maybe, because a compressor
   that flushes per record makes every boundary look like the last one. */
static inline bool ad_dvzip_end_ok(int e) { return e == AD_DVZIP_END_RECORD || e == AD_DVZIP_END_EXACT; }

static inline void ad_dvzip_begin(struct AdDvzipIter* it, const uint8_t* buf, uint32_t len) {
  it->buf = buf;
  it->len = len;
  it->pos = 0;
  it->nrec = 0;
  it->end = AD_DVZIP_END_RUNNING;
  it->cut_hdr = it->cut_want = it->cut_have = 0;
}

/* Yields the next record, or returns false and leaves it->end saying why it stopped.
   The payload is NOT touched -- the caller inflates or copies it. */
static inline bool ad_dvzip_next(struct AdDvzipIter* it, struct AdDvzipRec* out) {
  if (it->end != AD_DVZIP_END_RUNNING) return false;
  if (it->pos == it->len) {
    it->end = AD_DVZIP_END_EXACT;
    return false;
  }
  if (it->pos + 4 > it->len) {
    it->end = AD_DVZIP_END_STRAND;
    return false;
  }
  const uint8_t* p = it->buf + it->pos;
  uint32_t hdr = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
  it->pos += 4;
  if (hdr == 0) {
    it->end = AD_DVZIP_END_RECORD;
    return false;
  }
  uint32_t n = ad_dvzip_reclen(hdr);
  if (n > it->len - it->pos) {
    it->end = AD_DVZIP_END_CUT;
    it->cut_hdr = hdr;
    it->cut_want = n;
    it->cut_have = it->len - it->pos;
    it->pos -= 4; /* leave pos ON the header, so it can be dumped */
    return false;
  }
  out->hdr = hdr;
  out->off = it->pos;
  out->len = n;
  out->stored = ad_dvzip_is_flagged(hdr);
  it->pos += n;
  it->nrec++;
  return true;
}

/* ------------------------------------------------------------------------------------
 * Streaming drain: consume complete records from the FRONT of a buffer that is still
 * being filled.
 *
 * WHY. The receive path used to buffer the whole compressed body, answer 200, and only
 * then decode -- so a PSRAM exhaustion during decode happened AFTER the only moment it
 * could have been reported, and the sender was told "success" about a transfer this
 * device had already lost. Decoding record by record WHILE the body arrives moves every
 * decode failure to before the response, where it can be answered honestly. Records are
 * safe to decode in isolation because each is self-contained: a complete zlib stream,
 * or literal bytes (bit 31) -- largest compressed payload measured 131,118 B at the
 * 128 KiB grain.
 *
 * THE HOLD-BACK RULE, which is the one subtlety: the last complete record is NOT
 * consumed until at least one byte follows it (or `final` is set). The completion probe
 * (dvzip_complete in ad_http.h) judges the UNCONSUMED bytes, and it can only recognise
 * "boundary-aligned, maybe done" if a record is still there to land on, and "end
 * record, certainly done" if the zero header is still there to read. The zero record is
 * never consumed for the same reason. So a consuming caller's residue is bounded by one
 * complete record plus one partial record (~262 KiB at the measured grain) and is never
 * empty once anything has arrived -- which is exactly the invariant dvzip_complete was
 * written against, so its verdict on the residue equals its verdict on the whole body.
 *
 * `emit` is called once per consumed record with its in-buffer payload; returning false
 * stops the walk and the record is NOT counted as consumed, so no bytes are lost.
 * Returns the bytes consumed from buf[0..], whole records only -- buf+consumed is again
 * a record boundary and the caller compacts (memmove) by that much. *end reports why
 * the walk stopped: AD_DVZIP_END_RUNNING when this call stopped it (hold-back or emit
 * refusal), the iterator's verdict otherwise. Mid-stream, END_CUT is EXPECTED -- the
 * tail record simply has not finished arriving -- and only a `final` walk may read it
 * as damage. */
typedef bool (*ad_dvzip_emit_fn)(void* ctx, const struct AdDvzipRec* rec, const uint8_t* payload);

static inline uint32_t ad_dvzip_drain(const uint8_t* buf, uint32_t len, bool final, ad_dvzip_emit_fn emit, void* ctx,
                                      int* end) {
  struct AdDvzipIter it;
  struct AdDvzipRec rec;
  ad_dvzip_begin(&it, buf, len);
  uint32_t consumed = 0;
  while (ad_dvzip_next(&it, &rec)) {
    if (!final && it.pos >= len) break; /* hold back the last complete record */
    if (!emit(ctx, &rec, buf + rec.off)) break;
    consumed = it.pos;
  }
  if (end) *end = it.end; /* RUNNING iff this call stopped the walk, not the stream */
  return consumed;
}

/* Cheap shape test, used to tell a DvZip body from a gzip or a bare cpio. Deliberately
   only looks at the FIRST record: a body whose later records we cannot interpret is still
   a DvZip body, and calling it "not a cpio/gzip" instead would be a worse diagnosis. */
static inline bool ad_dvzip_looks_like(const uint8_t* in, uint32_t len) {
  if (len < 10) return false;
  uint32_t first = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) | (uint32_t)in[3];
  if (first == 0 || first > len - 4) return false;
  if (in[4] != 0x78) return false;                     /* zlib CMF */
  return ((((uint32_t)in[4] << 8) | in[5]) % 31) == 0; /* CMF/FLG check value */
}

#endif /* AD_DVZIP_H */
