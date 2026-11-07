// AirDrop HTTP/1.1 stream layer: the chunked reader and everything that decides
// where a request boundary is.
//
// Dependency-free integer C over a single abstract byte source, so the SAME code
// compiles into the firmware and into a host test (tools/test-http.sh) that replays
// scripted byte streams with scripted arrival TIMES. The timing is the point: every
// bug this layer has produced was a timeout interacting with chunked framing, and
// none of them is reachable without modelling when bytes arrive.
//
// ---------------------------------------------------------------------------
// THE INVARIANT
// ---------------------------------------------------------------------------
// A read that consumes bytes and then stops for any reason OTHER than the parser's
// own delimiter -- a timeout, an EOF, a short read, a line with no terminator --
// has left the byte stream at an unknown offset. TLS provides no framing above it
// to resynchronise against, so the stream is finished: the only legal operation
// afterwards is close. That is `poisoned`, and it is enforced HERE, in the reader,
// not at the call sites -- the bug that motivated it was created by tuning a
// timeout at a call site, and a rule living at call sites is a rule the next
// timeout tweak can quietly reopen.
//
// What that bug was, for the record. sharingd sends /Ask with a ~129KB chunked body
// (a PKCS#7 Apple-ID validation record plus a FileIcon preview). Answering /Ask and
// then draining that body with a 400ms budget read ZERO bytes -- the body had not
// begun arriving, the link's RTT being 0.4-1.0s -- and the code then looped and read
// the body's first chunk header, "1F7BB", as an HTTP request line. It 404'd into the
// middle of somebody's body and closed. The transfer died with the badge showing
// "Receiving 0kB" while the Mac showed the send as complete.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ad_dvzip.h" /* record framing: dvzip_complete below judges with it */

#define HTTP_BUF 2048

// A byte source. read() returns >0 bytes read, 0 on TIMEOUT (nothing arrived within
// the caller's budget -- NOT a close), and <0 when the peer has closed. Conflating
// the middle case with the last one is what latched EOF on a deliberately short
// drain and killed the connection the next request was about to arrive on.
struct HttpRdr;
typedef int (*http_src_read_fn)(struct HttpRdr* r, unsigned char* buf, int len);

struct HttpRdr {
  void* ctx;  // mbedtls_ssl_context* in the firmware; a script on the host
  http_src_read_fn src;
  uint32_t timeout_ms;       // the caller's current patience, visible for tests/logs
  const unsigned char* pre;  // bytes already read past the headers (primed)
  int pre_n, pre_p;
  unsigned char buf[HTTP_BUF];
  int n, p;
  bool eof;           // the peer really closed: sticky, and correctly so
  bool poisoned;      // see THE INVARIANT above: close is the only way out
  bool timed_out;     // the last read expired: not a close
  uint32_t consumed;  // body bytes actually consumed, so a log line can say
                      // "gave up having read 0" at the moment it happens
};

static inline void http_rdr_init(struct HttpRdr* r, void* ctx, http_src_read_fn src) {
  memset(r, 0, sizeof(*r));
  r->ctx = ctx;
  r->src = src;
  r->timeout_ms = 3000;
}

static int hr_fill(struct HttpRdr* r) {
  if (r->p < r->n) return r->n - r->p;
  r->p = r->n = 0;
  if (r->pre_p < r->pre_n) {  // primed bytes first
    int take = r->pre_n - r->pre_p;
    if (take > HTTP_BUF) take = HTTP_BUF;
    memcpy(r->buf, r->pre + r->pre_p, take);
    r->pre_p += take;
    r->n = take;
    return r->n;
  }
  if (r->eof) return 0;
  int ret = r->src(r, r->buf, HTTP_BUF);
  if (ret == 0) {
    r->timed_out = true;
    return 0;
  }  // timeout: NOT a close
  if (ret < 0) {
    r->eof = true;
    return 0;
  }
  r->timed_out = false;
  r->n = ret;
  return r->n;
}

static int hr_read(struct HttpRdr* r, unsigned char* dst, int len) {
  int got = 0;
  while (got < len) {
    if (hr_fill(r) <= 0) break;
    int take = r->n - r->p;
    if (take > len - got) take = len - got;
    memcpy(dst + got, r->buf + r->p, take);
    r->p += take;
    got += take;
  }
  r->consumed += (uint32_t)got;
  if (got > 0 && got < len) r->poisoned = true;  // stopped mid-request
  return got;
}

// Returns the line length, 0 for a blank line, -1 when nothing was available.
// A line with bytes but no terminator poisons the stream: the old version returned
// it as though it were complete, which made a timeout in the middle of a header or
// a chunk-size line indistinguishable from a real one.
static int hr_line(struct HttpRdr* r, char* out, int outsz) {
  int i = 0;
  for (;;) {
    if (hr_fill(r) <= 0) {
      if (i) {
        r->poisoned = true;
        out[i] = 0;
      }
      return i ? i : -1;
    }
    char c = (char)r->buf[r->p++];
    if (c == '\n') {
      if (i && out[i - 1] == '\r') i--;
      out[i] = 0;
      return i;
    }
    if (i < outsz - 1) out[i++] = c;
  }
}

static void hr_skip(struct HttpRdr* r, uint32_t len) {
  unsigned char tmp[512];
  while (len > 0) {
    int t = (len < 512) ? (int)len : 512;
    int g = hr_read(r, tmp, t);
    if (g <= 0) break;
    len -= (uint32_t)g;
  }
}

// The per-chunk CRLF: two bytes that say for free whether we are still aligned with
// the sender's framing. Discarding them threw that away.
static bool hr_crlf(struct HttpRdr* r) {
  unsigned char c[2];
  if (hr_read(r, c, 2) != 2 || c[0] != '\r' || c[1] != '\n') {
    r->poisoned = true;
    return false;
  }
  return true;
}

// Read and discard a request body. Returns true only if it ended at its own
// delimiter; anything else has poisoned the stream.
static bool http_drain_body(struct HttpRdr* r, bool chunked, int clen) {
  if (chunked) {
    char ln[40];
    for (;;) {
      if (hr_line(r, ln, sizeof(ln)) < 0) return false;
      if (r->poisoned) return false;
      uint32_t csz = (uint32_t)strtoul(ln, 0, 16);
      if (csz == 0) {  // last chunk, then trailers
        for (;;) {
          int t = hr_line(r, ln, sizeof(ln));
          if (t <= 0) return t == 0;
        }
      }
      hr_skip(r, csz);
      if (r->poisoned || !hr_crlf(r)) return false;
    }
  }
  if (clen > 0) {
    hr_skip(r, (uint32_t)clen);
    return !r->poisoned;
  }
  return true;
}

// A request line is by definition "<VERB> <PATH> HTTP/x.y". Anything else means we
// are not at a request boundary -- we are reading the middle of a body -- and every
// byte after it is garbage. Detected here so it can never be answered into.
static bool http_reqline_ok(const char* s) {
  if (strncmp(s, "POST /", 6) != 0 && strncmp(s, "GET /", 5) != 0) return false;
  return strstr(s, " HTTP/1.") != 0;
}

// ---------------------------------------------------------------------------
// DvZip completion
// ---------------------------------------------------------------------------
// The /Upload body is a run of [BE32 length][that many bytes of zlib] records. An
// explicit zero-length record is structural certainty; running out exactly on a
// record boundary is only a MAYBE, because a compressor that flushes per record
// makes every intermediate boundary look like the end.
//   2 = certain, 1 = boundary-aligned (needs corroborating silence), 0 = incomplete.
// Walks with ad_dvzip.h's iterator rather than by hand. The hand-rolled walk this
// replaces was a second copy of the framing knowledge, and a second copy learns
// slowly: it had to be taught about stored records separately from the iterator
// that already knew (before that, one stored record made this return 0 for ever,
// so such a body could only ever end on the HTTP last-chunk marker or a timeout --
// measured, 6 of the 18 records in a 2.35 MB photo are stored).
static int dvzip_complete(const unsigned char* in, uint32_t len) {
  if (len < 4) return 0;
  struct AdDvzipIter it;
  struct AdDvzipRec rec;
  ad_dvzip_begin(&it, in, len);
  while (ad_dvzip_next(&it, &rec)) {
    /* A deflated record's payload must open with a zlib header; a stored one must not
       be asked to. Anything else means these bytes are not DvZip at all. */
    if (!rec.stored && in[rec.off] != 0x78) return 0;
  }
  if (it.end == AD_DVZIP_END_RECORD) return 2;
  return (it.end == AD_DVZIP_END_EXACT) ? 1 : 0;
}

// Growable sink for the received body. `grow` must make room for `need` total bytes
// and return false if it cannot; the firmware backs it with PSRAM, the host with
// realloc.
struct HttpSink {
  unsigned char* buf;
  uint32_t cap, len, max;
  void* ctx;
  bool (*grow)(struct HttpSink* s, uint32_t need);
  /* Called after every successful append, if set. Optional and may be NULL.
   *
   * It exists because "receiving" was a state with no evidence behind it. The byte
   * counter a caller publishes was assigned once, AFTER the whole body had been read, so
   * a 73-second transfer displayed "0 KB" for 73 seconds and a transfer that never
   * started displayed "Receiving" for ever. Progress that is only reported at the end is
   * not progress; it is a result.
   *
   * A progress hook MAY CONSUME. It is allowed to remove a decoded prefix of buf
   * (memmove the tail down, reduce len), which is how the streaming DvZip sink decodes
   * a body WHILE it arrives instead of after the response has already promised success.
   * http_recv_chunked re-reads buf/len/max after every call and must keep doing so:
   * caching len across the hook would break the consuming sink silently, so the contract
   * is pinned by a host test (tools/http_test.c, "consuming sink"). A consuming hook in
   * turn owes three things, all supplied by ad_dvzip_drain's hold-back rule: remove only
   * whole records, never remove a terminating zero record, and never leave the buffer
   * empty -- dvzip_complete below judges the residue, and its verdict equals its verdict
   * on the whole body only under those three. */
  void (*progress)(struct HttpSink* s);
};

enum {
  HTTP_RECV_OK_END_RECORD = 2,  // finished on an explicit DvZip end record
  HTTP_RECV_OK_SILENCE = 1,     // boundary-aligned and the sender then went quiet
  HTTP_RECV_OK_LAST_CHUNK = 3,  // finished on the HTTP last-chunk marker
  HTTP_RECV_TRUNCATED = 0,      // ran out: what we have may be incomplete
  HTTP_RECV_OVERFLOW = -1,
  HTTP_RECV_POISONED = -2,
};

// Receive a chunked body into `sink`. `settle_ms` is how long the sender must stay
// quiet before a boundary-aligned DvZip stream is accepted as complete. Set it from
// the link, not from optimism: measured stalls here are 3004ms and 4104ms, and
// concluding "complete" during one returns success on a truncated body whose DvZip
// prefix still decompresses -- a corrupt image reported as a successful transfer.
// `set_timeout` lets the caller retune the source's patience per phase.
static int http_recv_chunked(struct HttpRdr* r, struct HttpSink* sink, uint32_t body_ms, uint32_t settle_ms,
                             void (*set_timeout)(struct HttpRdr*, uint32_t)) {
  char ln[40];
  bool have_hdr = false;  // a chunk header already read during a settle window
  for (;;) {
    // ONE place that reads a chunk header, ONE that appends, ONE that judges
    // completeness. The first version had a second append inside the settle branch
    // and did not re-judge after it, so a DvZip end record arriving as the chunk
    // right after a boundary-aligned probe was consumed and never recognised -- the
    // receiver then waited out the body timeout on a body it already had complete.
    // The host tests caught that in code already flashed to the device.
    if (!have_hdr) {
      if (set_timeout) set_timeout(r, body_ms);
      if (hr_line(r, ln, sizeof(ln)) < 0) return HTTP_RECV_TRUNCATED;
      if (r->poisoned) return HTTP_RECV_POISONED;
    }
    have_hdr = false;

    uint32_t csz = (uint32_t)strtoul(ln, 0, 16);
    if (csz == 0) return HTTP_RECV_OK_LAST_CHUNK;
    /* CHECK THE CHUNK SIZE BEFORE ADDING IT TO ANYTHING.
     *
     * csz comes from a hex line the sender wrote, so it can be anything. Written as
     * "sink->len + csz > sink->max" the addition WRAPS: a header of FFFC0000 makes the sum
     * small, the max guard passes, grow is skipped, and hr_read is called with (int)csz
     * negative. No memory is written -- hr_read does nothing with a negative length -- but
     * the framing desynchronises and the transfer dies, which is a denial of service a
     * stranger's device can trigger at will. Subtracting instead of adding cannot wrap,
     * because the len > max case is tested first. AND IT HAS TO BE: len <= max does NOT
     * always hold. A consuming progress hook signals "stop receiving" by setting max to 0
     * while the residue is still there, and with a bare subtraction 0 - len underflows to
     * four billion, the guard passes, and the abort never happens -- so the fix for one
     * unsigned bug quietly created another. Both were caught by reading, then pinned by
     * tests. */
    if (sink->len > sink->max || csz > sink->max - sink->len) return HTTP_RECV_OVERFLOW;
    if (sink->len > sink->cap || csz > sink->cap - sink->len) {
      if (!sink->grow(sink, sink->len + csz)) return HTTP_RECV_OVERFLOW;
    }
    if (set_timeout) set_timeout(r, body_ms);
    int got = hr_read(r, sink->buf + sink->len, (int)csz);
    if (got > 0) {
      sink->len += (uint32_t)got;
      if (sink->progress) sink->progress(sink);
    }
    if (got < (int)csz) return r->poisoned ? HTTP_RECV_POISONED : HTTP_RECV_TRUNCATED;
    if (!hr_crlf(r)) return HTTP_RECV_POISONED;

    int dvz = dvzip_complete(sink->buf, sink->len);
    if (dvz == 2) return HTTP_RECV_OK_END_RECORD;  // structural certainty
    if (dvz != 1) continue;                        // definitely more to come

    // Boundary-aligned: only the sender's silence can distinguish "done" from
    // "flushed a record". If something does arrive, keep its header and process it
    // through the normal path above rather than duplicating that path here.
    if (set_timeout) set_timeout(r, settle_ms);
    int lr = hr_line(r, ln, sizeof(ln));
    if (lr < 0) return HTTP_RECV_OK_SILENCE;
    if (r->poisoned) return HTTP_RECV_POISONED;
    have_hdr = true;
  }
}
