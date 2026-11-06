/* Host tests for awdl_serve.h -- the dispatch layer the firmware compiles.
 *
 * Same idea as http_test.c: the byte source is scripted in content AND TIME, and
 * the transport / clock / side effects are behind callbacks so the SAME dispatch
 * code runs here as on the device. What this harness adds over http_test.c is a
 * WRITE capture and a LOG capture, because the things worth asserting about the
 * dispatch layer are its outputs: which HTTP response bytes it emits, in what
 * order relative to draining, and which desync/idle events it logs.
 *
 *   cc -O2 -o /tmp/serve_test tools/serve_test.c && /tmp/serve_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../../vendor/esp32drop/src/airdrop/core/ad_http.h"
#include "../../vendor/esp32drop/src/airdrop/core/ad_serve.h"

/* ---- scripted byte source (content + arrival time), as in http_test.c ---- */
#define MAXSEG 64
typedef struct {
  uint32_t at_ms;
  const unsigned char* p;
  int n;
} Seg;
typedef struct {
  Seg seg[MAXSEG];
  int nseg, cur, off;
  uint32_t now_ms;
  int close_at_end;
} Script;

static int script_read(struct HttpRdr* r, unsigned char* buf, int len) {
  Script* s = (Script*)r->ctx;
  uint32_t deadline = s->now_ms + r->timeout_ms;
  for (;;) {
    if (s->cur >= s->nseg) {
      if (s->close_at_end) return -1;
      s->now_ms = deadline;
      return 0;
    }
    Seg* g = &s->seg[s->cur];
    if (g->at_ms > deadline) {
      s->now_ms = deadline;
      return 0;
    }
    if (g->at_ms > s->now_ms) s->now_ms = g->at_ms;
    int avail = g->n - s->off;
    int take = (avail < len) ? avail : len;
    memcpy(buf, g->p + s->off, take);
    s->off += take;
    if (s->off >= g->n) {
      s->cur++;
      s->off = 0;
    }
    return take;
  }
}
static void script_add(Script* s, uint32_t at, const void* p, int n) {
  s->seg[s->nseg].at_ms = at;
  s->seg[s->nseg].p = (const unsigned char*)p;
  s->seg[s->nseg].n = n;
  s->nseg++;
}

/* ---- test harness state: everything the dispatch layer reaches out to ---- */
typedef struct {
  Script* s; /* the clock lives in the script */
  char out[8192];
  int outn; /* captured response bytes */
  char logs[64][192];
  int nlog;
  uint32_t disc_ms, ask_ms, disc_ok, ask_ok, desync;
  int ask_sent, uploads;
  uint32_t peer_at_ms; /* when the upload connection shows up; UINT32_MAX = never.
                          0 means it is ALREADY in the backlog, which is the ordinary
                          case: sharingd opens it the instant it reads our 200. */
  int peer_polls;
  int up_chunked, up_clen; /* what the upload hook was handed */
  int upload_drains;       /* 1 = the hook actually drained the body */
} Harness;

static int fail_write = 0, write_calls = 0, response_failures = 0;
static void hz_response_failed(struct HttpServe* sv) {
  (void)sv;
  response_failures++;
}

static int hz_write(struct HttpServe* sv, const unsigned char* p, int n) {
  if (++write_calls == fail_write) return n - 1;
  Harness* h = (Harness*)sv->io;
  if (h->outn + n < (int)sizeof(h->out)) {
    memcpy(h->out + h->outn, p, n);
    h->outn += n;
  }
  return n;
}
static uint32_t hz_now(struct HttpServe* sv) { return ((Harness*)sv->io)->s->now_ms; }
static void hz_log(struct HttpServe* sv, const char* line) {
  Harness* h = (Harness*)sv->io;
  if (h->nlog < 64) {
    snprintf(h->logs[h->nlog], 192, "%s", line);
    h->nlog++;
  }
}
static void hz_set_timeout(struct HttpRdr* r, uint32_t ms) { r->timeout_ms = ms; }
static void hz_on_ask_sent(struct HttpServe* sv) { ((Harness*)sv->io)->ask_sent++; }
/* Stands in for a zero-timeout look at the listening socket. */
static bool hz_peer_waiting(struct HttpServe* sv) {
  Harness* h = (Harness*)sv->io;
  h->peer_polls++;
  return h->peer_at_ms != UINT32_MAX && h->s->now_ms >= h->peer_at_ms;
}

/* The upload hook models the firmware's rx_upload closely enough to test the
 * dispatch contract: it records what it was handed, drains the body through the
 * SAME awdl_http.h de-chunker the firmware uses, and answers 200. (The decode
 * pipeline itself is out of scope -- http_test.c already covers http_recv_chunked;
 * here we only assert dispatch handed it the right framing and closed after.) */
static void hz_upload(struct HttpServe* sv, bool chunked, int clen) {
  Harness* h = (Harness*)sv->io;
  h->uploads++;
  h->up_chunked = chunked;
  h->up_clen = clen;
  if (chunked) {
    struct HttpSink sk;
    memset(&sk, 0, sizeof sk);
    static unsigned char buf[1u << 20];
    sk.buf = buf;
    sk.cap = sizeof buf;
    sk.max = sizeof buf;
    int rc = http_recv_chunked(sv->r, &sk, 10000, 3000, sv->set_timeout);
    h->upload_drains = (rc >= 0);
  } else if (clen > 0) {
    hr_skip(sv->r, (uint32_t)clen);
    h->upload_drains = !sv->r->poisoned;
  }
  const char* ok = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  sv->write(sv, (const unsigned char*)ok, (int)strlen(ok));
}

static int consent = -1, authorized = -1;
static int metadata_mode = 0, consent_calls = 0;
static const char* gate_metadata(struct HttpServe* sv, bool chunked, int clen) {
  unsigned char body[8];
  uint32_t len;
  if (!http_read_ask(sv, chunked, clen, body, sizeof(body), &len)) return "400 Bad Request";
  return metadata_mode == 2 ? "413 Content Too Large" : NULL;
}
static bool gate_consent(struct HttpServe* s) {
  (void)s;
  consent_calls++;
  return consent == 1;
}
static bool gate_upload(struct HttpServe* s) {
  (void)s;
  return authorized == 1;
}
static bool logs_enabled = true;
static void harness_run_peer(Harness* h, Script* s, uint32_t peer_at_ms, bool wire_peer) {
  memset(h, 0, sizeof *h);
  h->s = s;
  h->peer_at_ms = peer_at_ms;
  struct HttpRdr r;
  http_rdr_init(&r, s, script_read);
  struct HttpServe sv;
  memset(&sv, 0, sizeof sv);
  sv.r = &r;
  sv.io = h;
  sv.write = hz_write;
  sv.now = hz_now;
  sv.log = logs_enabled ? hz_log : NULL;
  sv.set_timeout = hz_set_timeout;
  static const unsigned char DISC[] = "DISCBODY";
  static const unsigned char ASK[] = "ASKBODY";
  sv.disc_body = DISC;
  sv.disc_len = (int)sizeof DISC - 1;
  sv.ask_body = ASK;
  sv.ask_len = (int)sizeof ASK - 1;
  sv.disc_ms = &h->disc_ms;
  sv.ask_ms = &h->ask_ms;
  sv.disc_ok = &h->disc_ok;
  sv.ask_ok = &h->ask_ok;
  sv.desync = &h->desync;
  sv.on_ask_sent = hz_on_ask_sent;
  sv.response_failed = hz_response_failed;
  sv.upload = hz_upload;
  if (wire_peer) sv.peer_waiting = hz_peer_waiting;
  if (metadata_mode) sv.prepare_ask = gate_metadata;
  if (consent >= 0) sv.approve = gate_consent;
  if (authorized >= 0) sv.authorized = gate_upload;
  sv.enter_ms = s->now_ms;
  serve_http_run(&sv);
}
static void harness_run(Harness* h, Script* s) { harness_run_peer(h, s, 0, true); }

static int has_log(Harness* h, const char* needle) {
  for (int i = 0; i < h->nlog; i++)
    if (strstr(h->logs[i], needle)) return 1;
  return 0;
}

/* ---- DvZip / chunk builders (as in http_test.c) ------------------------- */
static int dvzip_record(unsigned char* out, int payload) {
  out[0] = (unsigned char)(payload >> 24);
  out[1] = (unsigned char)(payload >> 16);
  out[2] = (unsigned char)(payload >> 8);
  out[3] = (unsigned char)payload;
  out[4] = 0x78;
  out[5] = 0x9c;
  for (int i = 6; i < 4 + payload; i++) out[i] = (unsigned char)(i & 0xff);
  return 4 + payload;
}
static int chunk_wrap(unsigned char* out, const unsigned char* p, int n) {
  int k = sprintf((char*)out, "%X\r\n", n);
  memcpy(out + k, p, n);
  k += n;
  out[k++] = '\r';
  out[k++] = '\n';
  return k;
}

static int fails = 0, ran = 0;
static void check(const char* name, int ok, const char* detail) {
  ran++;
  printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

int main(void) {
  printf("== awdl_serve.h host tests (dispatch layer) ==\n");

  /* 1. /Discover: answers 200 with the disc body, and the body drain happens
     AFTER the response bytes (the ordering that fixed the 10,005ms exchange). */
  {
    static const char req[] =
        "POST /Discover HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nHELLO\r\n0\r\n\r\n";
    Script s;
    memset(&s, 0, sizeof s);
    script_add(&s, 0, req, (int)strlen(req));
    Harness h;
    harness_run(&h, &s);
    char* body = strstr(h.out, "\r\n\r\n");
    int ordered = body && strstr(body, "DISCBODY");
    int rsp200 = strncmp(h.out, "HTTP/1.1 200 OK", 15) == 0;
    char d[160];
    snprintf(d, sizeof d, "disc_ok=%u out=%.24s... has_body=%d", h.disc_ok, h.out, ordered ? 1 : 0);
    check("/Discover answers 200 + body and consumes the chunked body",
          rsp200 && ordered && h.disc_ok == 1 && has_log(&h, "disc_sent"), d);
  }

  /* 2. /Ask: answers 200 with Connection: close, flips the UI hook, does NOT
     invoke the upload pipeline, and closes (one response only). */
  {
    static const char req[] =
        "POST /Ask HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nWORLD\r\n0\r\n\r\n";
    Script s;
    memset(&s, 0, sizeof s);
    script_add(&s, 0, req, (int)strlen(req));
    Harness h;
    harness_run(&h, &s);
    int has_close = strstr(h.out, "Connection: close") != NULL;
    char d[160];
    snprintf(d, sizeof d, "ask_ok=%u ask_sent=%d uploads=%d close=%d", h.ask_ok, h.ask_sent, h.uploads, has_close);
    check("/Ask answers 200 close, flips UI, no upload",
          h.ask_ok == 1 && h.ask_sent == 1 && h.uploads == 0 && has_close, d);
  }

  /* 2b. THE /Ask DRAIN. The failure this exists for is measured on hardware:
     a 2.4 MB photo carries a 122,543-byte /Ask body and transfers; a 3.9 MB one carries
     214,480 and silently never starts; the sending Mac's socket send buffer is 131,072.
     sharingd posts its receive only after its whole send is queued, so a body that does
     not fit means it never reads the 200 we sent in millisecond three, and it dies on its
     own 45 s timer while every counter on our side says success.

     The old code drained for a fixed 1500 ms. At the 5.5 KB/s this link's 5,760-byte
     window allows, that is about 8 KB -- it never once drained a body, in fifteen captures.
     The drain now ends on the sender coming back, which is the exact moment it has read
     our 200. */
  {
    /* A body far larger than a 1500 ms window could have absorbed, arriving steadily. */
    static unsigned char req[40000];
    int w = 0;
    w += sprintf((char*)req, "POST /Ask HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
    static unsigned char blob[4096];
    memset(blob, 'x', sizeof blob);
    for (int i = 0; i < 8; i++) w += chunk_wrap(req + w, blob, (int)sizeof blob);
    w += sprintf((char*)req + w, "0\r\n\r\n");
    Script s;
    memset(&s, 0, sizeof s);
    script_add(&s, 0, req, w);
    Harness h;
    harness_run_peer(&h, &s, UINT32_MAX, true); /* peer never arrives in this run */
    char d[200];
    snprintf(d, sizeof d, "polls=%d log=%s", h.peer_polls, h.nlog ? h.logs[h.nlog - 1] : "-");
    check("the drain keeps reading past 1500 ms instead of stopping there",
          has_log(&h, "ask_drain") && !has_log(&h, "binned=0"), d);
    check("...and it consumes the whole body rather than a window of it",
          has_log(&h, "why=eof") || has_log(&h, "why=cap"), d);
    check("...and it polls the listener while it drains", h.peer_polls > 1, d);
  }
  {
    /* The sender comes back: that is the signal, and it must end the drain at once. */
    static unsigned char req[40000];
    int w = 0;
    w += sprintf((char*)req, "POST /Ask HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
    static unsigned char blob[4096];
    memset(blob, 'y', sizeof blob);
    for (int i = 0; i < 8; i++) w += chunk_wrap(req + w, blob, (int)sizeof blob);
    Script s;
    memset(&s, 0, sizeof s);
    script_add(&s, 0, req, w);
    Script s2;
    memset(&s2, 0, sizeof s2);
    script_add(&s2, 0, req, w);
    Harness h2;
    harness_run_peer(&h2, &s2, 0, true); /* already in the backlog */
    char d[200];
    snprintf(d, sizeof d, "log=%s", h2.nlog ? h2.logs[h2.nlog - 1] : "-");
    check("a peer already in the backlog ends the drain immediately", has_log(&h2, "why=peer"), d);
    check("...having consumed almost nothing, so a normal transfer pays nothing for this", has_log(&h2, "binned=0"), d);
  }
  {
    /* No predicate wired at all: the drain must still terminate, not hang. */
    static const char req[] = "POST /Ask HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nWORLD\r\n0\r\n\r\n";
    Script s;
    memset(&s, 0, sizeof s);
    script_add(&s, 0, req, (int)strlen(req));
    s.close_at_end = 1;
    Harness h;
    harness_run_peer(&h, &s, UINT32_MAX, false);
    char d[200];
    snprintf(d, sizeof d, "log=%s", h.nlog ? h.logs[h.nlog - 1] : "-");
    check("a NULL peer_waiting still terminates -- on eof or the cap, never a hang",
          has_log(&h, "ask_drain") && (has_log(&h, "why=eof") || has_log(&h, "why=cap")), d);
  }

  /* 3. The headline desync: /Ask kept alive, then the sender's /Ask body chunk
     header "1F7BB" arrives where the NEXT request line would be. It must be
     rejected as a request line (DESYNC), never answered into with a 404.
     Here we simulate the post-/Ask state directly: feed a lone chunk header as
     the first "request line". */
  {
    static const char req[] = "1F7BB\r\n"; /* the real chunk header seen in the field */
    Script s;
    memset(&s, 0, sizeof s);
    s.close_at_end = 1;
    script_add(&s, 0, req, (int)strlen(req));
    Harness h;
    harness_run(&h, &s);
    int no_404 = strstr(h.out, "404") == NULL;
    char d[160];
    snprintf(d, sizeof d, "desync=%u out_empty=%d logged=%d", h.desync, h.outn == 0, has_log(&h, "DESYNC"));
    check("a chunk header as a request line -> DESYNC, no 404 written",
          h.desync == 1 && no_404 && has_log(&h, "DESYNC"), d);
  }

  /* 4. /Upload: dispatch hands the chunked body to the upload hook, which drains
     it cleanly, and the connection closes after (upload is the last request). */
  {
    static unsigned char rec[512], wire[4096];
    char req[8192];
    int rn = dvzip_record(rec, 64);
    static const unsigned char endrec[4] = {0, 0, 0, 0};
    int w = 0;
    w += chunk_wrap(wire + w, rec, rn);
    w += chunk_wrap(wire + w, endrec, 4);
    int hn = sprintf(req, "POST /Upload HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
    memcpy(req + hn, wire, w);
    hn += w;
    Script s;
    memset(&s, 0, sizeof s);
    script_add(&s, 0, req, hn);
    Harness h;
    harness_run(&h, &s);
    int rsp200 = strstr(h.out, "200 OK") != NULL;
    char d[160];
    snprintf(d, sizeof d, "uploads=%d chunked=%d drained=%d rsp200=%d", h.uploads, h.up_chunked, h.upload_drains,
             rsp200);
    check("/Upload routes to the upload hook, drains, answers 200",
          h.uploads == 1 && h.up_chunked == 1 && h.upload_drains && rsp200, d);
  }

  /* 5. A well-formed but unknown path -> 404 (the ONLY case a 404 is legal). */
  {
    static const char req[] = "POST /Nope HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    Script s;
    memset(&s, 0, sizeof s);
    s.close_at_end = 1;
    script_add(&s, 0, req, (int)strlen(req));
    Harness h;
    harness_run(&h, &s);
    int is404 = strstr(h.out, "404 Not Found") != NULL;
    char d[120];
    snprintf(d, sizeof d, "out=%.20s... desync=%u", h.out, h.desync);
    check("unknown POST path -> 404 (and NOT counted as desync)", is404 && h.desync == 0 && has_log(&h, "http_404"), d);
  }

  /* 6. First request line never arrives (peer silent), then closes: idle_end,
     no response, no desync. This is the "waiting for /Ask that never comes"
     state whose budget was the 3000ms regression -- here we only assert the
     clean give-up, and that it waited the FIRST-request budget (10000ms). */
  {
    Script s;
    memset(&s, 0, sizeof s);
    s.close_at_end = 0; /* silence forever */
    Harness h;
    harness_run(&h, &s);
    char d[140];
    snprintf(d, sizeof d, "clock=%ums out_empty=%d idle=%d", s.now_ms, h.outn == 0, has_log(&h, "http_idle_end"));
    check("no request line -> idle_end after the 10s first-request budget, no output",
          h.outn == 0 && h.desync == 0 && s.now_ms == 10000 && has_log(&h, "http_idle_end"), d);
  }

  /* 7. Headers cut off mid-way (blank line never arrives, then close): the body
     and rest of the headers are still out there, so it must close as
     hdr_incomplete rather than proceed on partial headers. */
  {
    static const char req[] = "POST /Ask HTTP/1.1\r\nContent-Len"; /* truncated header */
    Script s;
    memset(&s, 0, sizeof s);
    s.close_at_end = 1;
    script_add(&s, 0, req, (int)strlen(req));
    Harness h;
    harness_run(&h, &s);
    char d[140];
    snprintf(d, sizeof d, "ask_ok=%u desync=%u logged=%d", h.ask_ok, h.desync, has_log(&h, "http_hdr_incomplete"));
    check("truncated headers -> hdr_incomplete close, /Ask NOT answered",
          h.ask_ok == 0 && h.desync == 1 && has_log(&h, "http_hdr_incomplete"), d);
  }

  /* 8. Expect: 100-continue -> a 100 Continue is emitted before the 200. */
  {
    static const char req[] = "POST /Discover HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 0\r\n\r\n";
    Script s;
    memset(&s, 0, sizeof s);
    script_add(&s, 0, req, (int)strlen(req));
    Harness h;
    harness_run(&h, &s);
    char* cont = strstr(h.out, "100 Continue");
    char* ok = strstr(h.out, "200 OK");
    char d[120];
    snprintf(d, sizeof d, "has100=%d has200=%d ordered=%d", cont != NULL, ok != NULL, (cont && ok && cont < ok));
    check("Expect: 100-continue -> 100 emitted before the 200", cont && ok && cont < ok, d);
  }

  {
    const char* ask = "POST /Ask HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    const char* upload = "POST /Upload HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    Script s = {0};
    Harness h;
    s.close_at_end = 1;
    consent = 0;
    script_add(&s, 0, ask, strlen(ask));
    harness_run(&h, &s);
    check("declined /Ask returns403 without accepting", strstr(h.out, "403 Forbidden") && !h.ask_ok, NULL);
    memset(&s, 0, sizeof(s));
    s.close_at_end = 1;
    consent = 1;
    script_add(&s, 0, ask, strlen(ask));
    harness_run(&h, &s);
    check("accepted /Ask returns200", strstr(h.out, "200 OK") && h.ask_ok == 1, NULL);
    memset(&s, 0, sizeof(s));
    s.close_at_end = 1;
    authorized = 0;
    script_add(&s, 0, upload, strlen(upload));
    harness_run(&h, &s);
    check("unapproved upload rejected before handler", strstr(h.out, "403 Forbidden") && !h.uploads, NULL);
    memset(&s, 0, sizeof(s));
    s.close_at_end = 1;
    authorized = 1;
    script_add(&s, 0, upload, strlen(upload));
    harness_run(&h, &s);
    check("approved upload reaches handler", h.uploads == 1, NULL);
  }
  for (int mode = 1; mode <= 2; ++mode) {
    const char* ask = "POST /Ask HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc";
    Script s = {0};
    Harness h;
    s.close_at_end = 1;
    consent = 1;
    consent_calls = 0;
    metadata_mode = mode;
    script_add(&s, 0, ask, strlen(ask));
    harness_run(&h, &s);
    if (mode == 1)
      check("metadata consumed before consent and200 without drain",
            consent_calls == 1 && h.ask_ok == 1 && h.peer_polls == 0, NULL);
    else
      check("oversize metadata returns413 without consent or acceptance",
            consent_calls == 0 && !h.ask_ok && strstr(h.out, "413 Content Too Large"), NULL);
  }
  {
    Script s = {0};
    Harness h;
    const char* req = "POST /Discover HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    s.close_at_end = 1;
    script_add(&s, 0, req, strlen(req));
    logs_enabled = false;
    harness_run(&h, &s);
    check("receiver without diagnostics still replies", h.disc_ok == 1 && strstr(h.out, "200 OK") && h.nlog == 0, NULL);
  }
  for (int failure = 1; failure <= 2; ++failure) {
    Script s = {0};
    Harness h;
    const char* req = "POST /Ask HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc";
    s.close_at_end = 1;
    metadata_mode = 1;
    consent = 1;
    write_calls = response_failures = 0;
    fail_write = failure;
    script_add(&s, 0, req, strlen(req));
    harness_run(&h, &s);
    check("short acceptance response never starts a transfer",
          h.ask_ok == 0 && h.ask_sent == 0 && response_failures == 1 && h.uploads == 0, NULL);
  }
  fail_write = 0;
  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
