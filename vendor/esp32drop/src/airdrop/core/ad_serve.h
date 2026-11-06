// AirDrop HTTP/1.1 dispatch layer: everything between "a TLS session finished its
// handshake" and "a request body handler runs" -- request-line validation, the
// DESYNC detector, header parsing, the per-phase read budgets, and the response
// ordering rules for /Discover, /Ask and /Upload.
//
// Same contract as awdl_http.h: dependency-free integer C over the abstract
// HttpRdr, so the SAME code compiles into the firmware and into a host test
// (tools/test-serve.sh) that scripts both the bytes and their arrival times. The
// budgets in here (10000 / 800 / 3000 / 400 / 300+1500) each encode a measured
// failure; the comments at each site say which. Do not retune one without a test.
//
// What stays OUT of this file, on purpose:
//   - the transport (writes go through sv->write; the firmware backs it with
//     ssl_write_all, the host test with a capture buffer)
//   - time (sv->now; virtual on the host -- every historical bug in this stack
//     was a timeout interacting with framing, so time must be scriptable)
//   - firmware side effects (counters, UI flips, the upload pipeline) -- those
//     are pointer-fields and hooks so the .ino keeps owning its globals
#pragma once
/* G2 measurement hook -- see AD_G2_READ_BODIES below. Off unless the build asks. */
#ifndef AD_G2_BODY_MS
#define AD_G2_BODY_MS 20000
#endif
#include <stdio.h>

#include "ad_http.h"

struct HttpServe;
typedef void (*http_settimeout_fn)(struct HttpRdr* r, uint32_t ms);
typedef int (*http_write_fn)(struct HttpServe* s, const unsigned char* p, int n);
typedef uint32_t (*http_now_fn)(struct HttpServe* s);
typedef void (*http_logline_fn)(struct HttpServe* s, const char* line);
typedef void (*http_upload_fn)(struct HttpServe* s, bool chunked, int clen);
typedef void (*http_hook_fn)(struct HttpServe* s);
typedef const char* (*http_ask_fn)(struct HttpServe*, bool chunked, int clen);
/* "Has the sender come back for /Upload yet?" -- a zero-timeout look at the listening
   socket. Optional; NULL falls back to a bounded wait. It is the signal that the /Ask
   drain below is finished, and it is exact: sharingd opens the upload connection the
   moment it has read our 200, on the very next ephemeral port (ask_port+1 in all twelve
   captured successes). */
typedef bool (*http_pred_fn)(struct HttpServe* s);

struct HttpServe {
  struct HttpRdr* r;
  void* io;             // opaque for the callbacks
  http_write_fn write;  // full-buffer write; <0 on failure
  http_now_fn now;      // firmware: millis(); host: the script's clock
  http_logline_fn log;  // one preformatted event line (no timestamp, no \n)
  http_settimeout_fn set_timeout;

  const unsigned char* disc_body;
  int disc_len;  // the /Discover 200 payload
  const unsigned char* ask_body;
  int ask_len;  // the /Ask 200 payload

  // Cross-connection clocks and counters. Pointers, not values: /Discover and
  // /Ask arrive on DIFFERENT connections, so their relationship ("ask_ok
  // since_disc=") lives outside any one serve call. The firmware points these at
  // its globals (volatile, since two tasks read them); the host test at locals.
  volatile uint32_t *disc_ms, *ask_ms;
  volatile uint32_t *disc_ok, *ask_ok, *desync;

  http_hook_fn response_failed;  // optional: revoke consent/report a failed write
  http_hook_fn on_ask_sent;      // fired right after the /Ask 200: UI flip etc.
  http_ask_fn prepare_ask;       // optional metadata read; NULL means success, else HTTP error
  http_pred_fn cancelled;        // optional stop signal while reading metadata
  http_pred_fn approve;          // optional user approval before /Ask succeeds
  http_pred_fn authorized;       // optional peer-bound authorization for /Upload
  http_pred_fn peer_waiting;     // optional: a connection is pending on the listener
  http_upload_fn upload;         // the /Upload body pipeline (firmware: rx_upload)

  uint32_t enter_ms;  // when the HTTP phase began (for http_idle_end)
};

static bool http_send_response(struct HttpServe* sv, const char* status, const unsigned char* body, int blen,
                               bool close_it) {
  char h[192];
  int hl =
      snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\n%s\r\n",
               status, blen, close_it ? "Connection: close\r\n" : "");
  if (sv->write(sv, (const unsigned char*)h, hl) != hl || (body && blen && sv->write(sv, body, blen) != blen)) {
    return false;
  }
  return true;
}

#define SV_LOG(sv, ...)                    \
  do {                                     \
    if (!(sv)->log) break;                 \
    char _l[192];                          \
    snprintf(_l, sizeof(_l), __VA_ARGS__); \
    (sv)->log((sv), _l);                   \
  } while (0)

// One underlying refill per iteration keeps the overall deadline effective even
// when a peer trickles bytes faster than the socket's per-read timeout.
static bool http_ask_read(struct HttpServe* sv, uint32_t start, uint8_t* out, uint32_t count) {
  struct HttpRdr* r = sv->r;
  while (count) {
    if ((sv->cancelled && sv->cancelled(sv)) || sv->now(sv) - start >= 20000) return false;
    int available = hr_fill(r);
    if (available <= 0 || (sv->cancelled && sv->cancelled(sv)) || sv->now(sv) - start >= 20000) return false;
    uint32_t n = count < (uint32_t)available ? count : (uint32_t)available;
    memcpy(out, r->buf + r->p, n);
    r->p += n;
    r->consumed += n;
    out += n;
    count -= n;
  }
  return true;
}
static int http_ask_line(struct HttpServe* sv, uint32_t start, char* out, uint32_t capacity) {
  uint32_t n = 0;
  while (n + 1 < capacity) {
    uint8_t c;
    if (!http_ask_read(sv, start, &c, 1)) return -1;
    if (c == '\r') {
      if (!http_ask_read(sv, start, &c, 1) || c != '\n') return -1;
      out[n] = 0;
      return (int)n;
    }
    if (c == '\n') return -1;
    out[n++] = (char)c;
  }
  return -1;
}
static bool http_ask_crlf(struct HttpServe* sv, uint32_t start) {
  uint8_t crlf[2];
  return http_ask_read(sv, start, crlf, 2) && crlf[0] == '\r' && crlf[1] == '\n';
}

// Strict, bounded metadata framing; unlike the upload reader, no DvZip silence heuristic.
static bool http_read_ask(struct HttpServe* sv, bool chunked, int clen, uint8_t* out, uint32_t cap, uint32_t* length) {
  struct HttpRdr* r = sv->r;
  *length = 0;
  const uint32_t start = sv->now(sv);
  sv->set_timeout(r, 2000);
  for (;;) {
    if ((sv->cancelled && sv->cancelled(sv)) || sv->now(sv) - start >= 20000) return false;
    uint32_t count = 0;
    if (chunked) {
      char line[40];
      int n = http_ask_line(sv, start, line, sizeof(line));
      if (n <= 0 || r->poisoned) return false;
      int digits = 0;
      for (int i = 0; i < n && line[i] != ';'; ++i) {
        const unsigned char c = line[i];
        const uint32_t d = c >= '0' && c <= '9'   ? c - '0'
                           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                  : 255;
        if (d > 15 || count > (UINT32_MAX - d) / 16) return false;
        count = count * 16 + d;
        ++digits;
      }
      if (!digits) return false;
      if (!count) return http_ask_crlf(sv, start) && !r->poisoned;
    } else {
      if (clen <= 0) return false;
      count = (uint32_t)clen;
    }
    if (count > cap - *length) return false;
    while (count) {
      if ((sv->cancelled && sv->cancelled(sv)) || sv->now(sv) - start >= 20000) return false;
      const uint32_t n = count < 4096 ? count : 4096;
      if (!http_ask_read(sv, start, out + *length, n) || r->poisoned) return false;
      *length += n;
      count -= n;
    }
    if (!chunked) return true;
    if (!http_ask_crlf(sv, start)) return false;
  }
}

static void serve_http_run(struct HttpServe* sv) {
  struct HttpRdr* r = sv->r;
  char line[512], reqline[128];
  int served = 0;
  for (;;) {
    // A poisoned stream is finished. Parsing anything more out of it is how a 404
    // got written into the middle of somebody's request body.
    if (r->poisoned) {
      SV_LOG(sv, "http_poisoned served=%d consumed=%lu -- stream offset unknown, closing", served,
             (unsigned long)r->consumed);
      (*sv->desync)++;
      break;
    }
    // Budget for waiting on a request line. Two very different states share this
    // read:
    //  - FIRST request on a freshly adopted connection: waiting is FREE (every
    //    other connection was closed at TLS-OK), and charging it 3000ms was a
    //    measured regression (`http_idle_end served=0 waited_ms=3001 why=timeout`
    //    while the sender's compound latency ran to ~4s). v25 used a flat 10000ms
    //    here and worked. So 10000ms.
    //  - AFTER serving: the task is deaf to accept() while it waits, so it must
    //    give up fast. (`served > 0` is unreachable today -- every handler below
    //    closes -- but the branch is kept so a future keep-alive handler cannot
    //    silently inherit the wrong budget.)
    sv->set_timeout(r, served ? 800 : 10000);
    int L = hr_line(r, reqline, sizeof(reqline));  // request line
    if (L <= 0) {
      SV_LOG(sv, "http_idle_end served=%d waited_ms=%lu why=%s", served, (unsigned long)(sv->now(sv) - sv->enter_ms),
             r->eof ? "peer-closed" : (r->timed_out ? "timeout" : "short-read"));
      break;
    }
    // A request line is by definition "<VERB> <PATH> HTTP/x.y". Anything else means
    // we are not at a request boundary -- we are reading the middle of somebody's
    // body -- and every byte after it is garbage. Not recoverable on a single TLS
    // stream (no framing above it to resynchronise against), so say so loudly and
    // destroy the connection rather than 404 the garbage and carry on. This check
    // exists because the failure it detects was found by luck: a chunk header
    // happened to be "1F7BB", valid hex, and the request line happened to be
    // printed. tools/test-serve.sh pins it deliberately.
    if (strncmp(reqline, "POST /", 6) != 0 || !strstr(reqline, " HTTP/1.")) {
      char safe[40];
      int si2 = 0;
      for (const char* q = reqline; *q && si2 < 39; q++) safe[si2++] = (*q >= 32 && *q < 127) ? *q : '.';
      safe[si2] = 0;
      SV_LOG(sv, "DESYNC served=%d not-a-request-line=\"%s\" -- stream is out of frame, closing", served, safe);
      (*sv->desync)++;
      break;
    }
    sv->set_timeout(r, 3000);  // headers + body: bounded too
    bool is_disc = (strncmp(reqline, "POST /Discover", 14) == 0);
    bool is_ask = (strncmp(reqline, "POST /Ask", 9) == 0);
    bool is_up = (strncmp(reqline, "POST /Upload", 12) == 0);

    int clen = 0;
    bool chunked = false, expect100 = false;
    bool hdr_ok = true;
    for (;;) {  // headers up to the blank line
      int hl = hr_line(r, line, sizeof(line));
      if (hl == 0) break;  // the blank line: headers done
      if (hl < 0) {
        hdr_ok = false;
        break;
      }  // timeout/EOF mid-headers: the
         // rest of the headers and the
         // body are still out there
      if (strncasecmp(line, "Content-Length:", 15) == 0) {
        clen = atoi(line + 15);
      } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 && strstr(line, "chunked"))
        chunked = true;
      else if (strncasecmp(line, "Expect:", 7) == 0 && strstr(line, "100"))
        expect100 = true;
    }
    if (!hdr_ok || r->poisoned) {
      SV_LOG(sv, "http_hdr_incomplete req=\"%.24s\" eof=%d to=%d -- closing", reqline, r->eof, r->timed_out);
      (*sv->desync)++;
      break;
    }
    if (clen < 0) clen = 0;
    const uint32_t t_req = sv->now(sv);
    served++;
    SV_LOG(sv, "req %s clen=%d chunk=%d exp100=%d", reqline, clen, chunked, expect100);

    if (expect100) {
      const char* c = "HTTP/1.1 100 Continue\r\n\r\n";
      if (sv->write(sv, (const unsigned char*)c, (int)strlen(c)) != (int)strlen(c)) {
        if (is_ask && sv->response_failed) sv->response_failed(sv);
        break;
      }
    }

    if (is_up) {
      if (sv->authorized && !sv->authorized(sv)) {
        http_send_response(sv, "403 Forbidden", NULL, 0, true);
        break;
      }
      SV_LOG(sv, "upload_start since_ask=%lu", (unsigned long)(*sv->ask_ms ? (sv->now(sv) - *sv->ask_ms) : 0));
      sv->set_timeout(r, 10000);      // multi-MB body over a ~1s-RTT link: be patient
      sv->upload(sv, chunked, clen);  // de-chunk -> gunzip -> cpio -> main task
      break;                          // /Upload is the last request; close after
    }

#ifdef AD_G2_READ_BODIES
    /* G2 ONLY, and it deliberately breaks the shipping trade-off.
     *
     * /Discover and /Ask are answered WITHOUT reading their bodies, for reasons measured
     * and written out below and at the /Ask drain: the response does not depend on the
     * body, and waiting for it cost a measured 10,005 ms once. The consequence is stated
     * in the /Ask comment: "we have never read a single byte of this body, in the whole
     * life of the project" -- ask_linger binned=0 in all fifteen captured occurrences.
     *
     * That is measurement G2, and the send path cannot be written without it: a sender
     * has to PRODUCE these bodies, and airdrop_cert.h's blobs are receiver-shaped.
     * So this build reads them first and lets the port's HTTP_TRACE tap see them.
     *
     * NOT SHIPPABLE, and the default is off. Reading /Ask means pulling ~129 KB through a
     * 6.25 %-duty link before the transfer can start, which is exactly the cost the
     * shipping path exists to avoid, and it may push sharingd into its 45 s ASK timeout.
     * A measurement build may pay that; a user's badge may not. */
    if (is_disc || is_ask) {
      sv->set_timeout(r, AD_G2_BODY_MS);
      SV_LOG(sv, "G2: reading the body before answering (clen=%d chunk=%d)", clen, chunked);
      http_drain_body(r, chunked, clen);
      SV_LOG(sv, "G2: body read done poisoned=%d eof=%d to=%d", r->poisoned, r->eof, r->timed_out);
    }
#endif

    if (is_disc) {
      // Answer FIRST, drain afterwards. Draining first cost a measured 10,005ms on
      // one exchange: sharingd sends /Discover with Transfer-Encoding: chunked, and
      // when the segment carrying the body is lost, TCP recovery over this link is
      // slow -- our own ACKs can only leave during the one 65ms ch6 slot per 1048ms
      // cycle, so the effective RTT is ~1s and the retransmit backoff is measured in
      // seconds. The response does not depend on the body at all, so nothing is
      // gained by waiting for it, and by the time we answered the sender had given
      // up and torn the connection down. Now: respond, then make a brief best-effort
      // attempt to consume the body so the close is graceful rather than a RST.
      if (!http_send_response(sv, "200 OK", sv->disc_body, sv->disc_len, true)) break;
      (*sv->disc_ok)++;
      *sv->disc_ms = sv->now(sv);
      SV_LOG(sv, "disc_sent n=%lu ms_since_req=%lu -- tile should appear now", (unsigned long)*sv->disc_ok,
             (unsigned long)(sv->now(sv) - t_req));
      sv->set_timeout(r, 400);
      http_drain_body(r, chunked, clen);
      break;  // one-shot: close and get back to accept()
    }

    if (is_ask) {
      if (sv->prepare_ask) {
        const char* error = sv->prepare_ask(sv, chunked, clen);
        if (error) {
          http_send_response(sv, error, NULL, 0, true);
          break;
        }
      }
      if (sv->approve && !sv->approve(sv)) {
        http_send_response(sv, "403 Forbidden", NULL, 0, true);
        break;
      }
      // Metadata-aware receivers consumed the body before consent. Legacy users
      // still use the bounded post-response drain below. Upload uses a new connection.
      if (!http_send_response(sv, "200 OK", sv->ask_body, sv->ask_len, true)) {
        if (sv->response_failed) sv->response_failed(sv);
        break;
      }
      *sv->ask_ms = sv->now(sv);
      (*sv->ask_ok)++;
      if (sv->on_ask_sent) sv->on_ask_sent(sv);  // firmware: flip to "Receiving" UI
      SV_LOG(sv, "ask_ok since_disc=%lu ms_since_req=%lu conn=close",
             (unsigned long)(*sv->disc_ms ? (sv->now(sv) - *sv->disc_ms) : 0), (unsigned long)(sv->now(sv) - t_req));
      /* DRAIN UNTIL THE SENDER COMES BACK, not for a fixed 1500 ms.
       *
       * Measured on hardware, and this is the whole reason the drain exists:
       *
       *   a 2.4 MB photo -> /Ask body 122,543 B -> transfers
       *   a 3.9 MB photo -> /Ask body 214,480 B -> silently never starts
       *   macOS net.inet.tcp.sendspace on the sending Mac = 131,072 B
       *
       * 131,072 lies between them, and that is not a coincidence about file size. sharingd
       * hands the whole /Ask request to one send() and posts its receive only when the
       * LAST byte has been queued. If the request fits in its own socket send buffer the
       * send completes against local memory, it reads the 200 that has been waiting since
       * millisecond three, and opens /Upload -- then our close throws away the ~110 KB it
       * never transmitted. If the request does NOT fit, the send never completes, our 200
       * is delivered and ACKed but never read, and sharingd dies on its own 45 s timer:
       * "E [AirDropNW] ASK request timeout" in the Mac's log, and on our side nothing at
       * all -- ask_ok, binned=0, badge on "Receiving". That is the silent failure.
       *
       * So the old design worked by relying on the SENDER to abandon its own body, and the
       * working transfer had 8,529 bytes of margin. What decides the size is the preview
       * image, which tracks how detailed the picture is -- not how many bytes the file has.
       * An ordinary small file with a busy preview reaches the same cliff.
       *
       * We do NOT need to read the body. We need to read (body - sendspace), which is zero
       * for everything that works today. peer_waiting() is exactly that stopping point: it
       * goes true when sharingd has read our 200 and opened the upload connection. So a
       * transfer that never needed the drain pays one syscall for it.
       *
       * Nothing is PARSED here, so nothing can desynchronise. And reading before closing
       * still serves its original purpose: closing with unread data makes lwIP emit a RST,
       * and a RST can destroy data the peer has not yet handed to its application
       * (RFC 2525 2.17) -- including the 200 we just sent. */
      if (sv->prepare_ask) break;  // metadata reader consumed all framing
      sv->set_timeout(r, 300);
      {
        unsigned char scrap[512];
        uint32_t t0 = sv->now(sv), binned = 0;
        int tmo = 0;
        const char* why = "cap";
        for (;;) {
          if (sv->peer_waiting && sv->peer_waiting(sv)) {
            why = "peer";
            break;
          }
          if (r->eof) {
            why = "eof";
            break;
          }
          /* 30 s covers the worst case measured -- 83,408 bytes still to drain at the
             5.5 KB/s this link's 5,760-byte receive window allows, about 15 s -- with room
             to spare. It is a backstop, not the expected exit: peer_waiting ends it. */
          if ((uint32_t)(sv->now(sv) - t0) >= 30000) {
            why = "cap";
            break;
          }
          int g = hr_read(r, scrap, sizeof(scrap));
          if (g > 0)
            binned += (uint32_t)g;
          else
            tmo++; /* a timeout is not the end of anything on a 6.25%-duty link */
        }
        SV_LOG(sv, "ask_drain binned=%lu why=%s tmo=%d eof=%d ms=%lu", (unsigned long)binned, why, tmo, r->eof,
               (unsigned long)(sv->now(sv) - t0));
      }
      break;  // /Upload arrives on a fresh connection
    }

    http_drain_body(r, chunked, clen);  // consume the request body (chunked or CL)

    {
      SV_LOG(sv, "http_404 %s", reqline);
      http_send_response(sv, "404 Not Found", NULL, 0, true);
      break;
    }
  }
}
