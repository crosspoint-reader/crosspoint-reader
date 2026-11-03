/* ad_send.cpp -- see ad_send.h.
 *
 * The request bytes below are not invented. Every field, its type and its order came from
 * decoding a real macOS sender's own /Discover, /Ask and /Upload, and several of them fail
 * silently if changed: TransferID is a DICT and not a string (a string was closed on
 * without an answer), Items is an empty array, /Upload's TotalBytes is the FILE size and
 * not the body length (the body length made the peer wait for bytes that never came). What
 * is NOT here is equally measured: no client certificate, no SenderRecordData, no FileIcon,
 * no SenderIdentityAuthTag. All four were tried and none is required.
 *
 * The structure is a state machine because /Ask waits for a human. Each stage opens its own
 * connection -- as the real sender does -- and each poll advances one step of one stage.
 */

/* SENDER-ONLY, like ad_cycle.cpp and BLEAdvMin.cpp beside it -- and it was the one of the
 * three without the guard while ESP32Drop.h includes its header unconditionally.
 *
 * What that cost: this translation unit carries about 22 KB of static internal DRAM (a
 * 16 KB request buffer, a 3 KB response buffer, a bplist writer and the headers), and
 * internal RAM is the resource this library actually runs out of -- 2,088 bytes of added
 * static has already been enough, once, to stop mbedtls_ctr_drbg_seed() and make the
 * device silently undiscoverable. A receiver build has no business reserving any of it.
 *
 * The header stays unconditional on purpose: declarations cost nothing, and a caller that
 * uses them in a build without the define gets a link error naming the function it wanted,
 * which is a better answer than a compile error naming a missing header. */
#ifdef ESP32DROP_SENDER

#include "ad_send.h"

#include <Arduino.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/sockets.h>

#include "../ESP32AWDL.h" /* re-exports awdl/port; the layer rule wants the umbrella */
#include "ad_cycle.h"
#include "core/ad_bplist.h"
#include "core/ad_dvzip.h" /* AD_DVZIP_GRAIN: the 128 KiB record grain */
#include "core/ad_pack.h"
#include "core/ad_uti.h"
#include "core/ad_zlib.h"
#include "core/airdrop_cert.h"
#include "esp_heap_caps.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ssl.h"
#include "port/ad_port_esp32.h"

#define AD_SEND_PORT 8770

/* One in flight at a time, so the whole machine is one object. */
static struct {
  bool begun;
  int state, step, err, http_status, tls_err;
  int failed_in; /* the stage state held when fail() ran; 0 if none */
  bool airdropable, hold;
  char peer_name[64];
  uint8_t peer_mac[6];
  uint32_t sent, total, t_start, t_stage;
  uint32_t t_prog; /* last time the CURRENT step moved a byte. A request must be
                      bounded by STALL, never by elapsed time: /Upload's body
                      crosses a 5.7-26 KB/s link, so a 265 KB card legitimately
                      takes 46 s at the low end and a fixed budget would kill
                      exactly the transfers that were working.               */
  int sock_errno;  /* why the connect gave up: errno, or 0 for the timeout */

  int fd;
  bool tls_live;

  struct AdSendFile file;
  const char* file_uti; /* what /Ask declares. A string in ad_uti.h's table, or
                           the caller's own pointer -- borrowed either way. */
  char xfer_id[40];
  char sender_name[64];

  uint8_t *arch, *body; /* PSRAM */
  uint32_t arch_len, body_len, body_off, body_cap;
  uint8_t hdrs[320];
  uint32_t hdrs_len, hdrs_off;
  /* /Discover and /Ask bodies; /Upload streams from `body` in PSRAM instead.
   *
   * 2 KB, not the 16,384 this used to be. That figure was "big enough for an inline
   * FileIcon" -- and eighty lines below, build_ask() says FileIcon "does not help either
   * platform and is not sent". So 16 KB of the scarcest memory on the part was reserved
   * for something deliberately omitted.
   *
   * What the bodies actually measure, from tools/test-bplist.sh, which pins both byte-exact
   * against goldens: an /Ask-shaped document with Files as an array of dicts is 265 bytes,
   * and a flat /Discover dictionary is 110. 2 KB is over seven times the larger, which
   * leaves room for a long UTF-8 device name and a long filename without pretending to a
   * precision nobody has measured.
   *
   * Undersizing is safe, which is what makes shrinking it defensible: ad_bplist_dict() and
   * ad_bpw_finish() both RETURN 0 rather than overflow -- ad_bpw_finish refuses outright
   * below 48 bytes and bounds every write against end -- and the caller turns 0 into
   * ADS_ERR_NO_MEM. The failure mode is an honest error, not a corrupted body. */
  uint8_t reqbody[2048];
  uint32_t reqbody_len;
  bool chunk_hdr_sent, tail_sent;

  uint8_t rsp[3072];
  int rsp_got;
  int rsp_end; /* length of a complete response, 0 = not all here yet */
} g;

static mbedtls_entropy_context g_ent;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_ssl_config g_conf;
static mbedtls_ssl_context g_ssl;

/* ---- bio ---------------------------------------------------------------------------- */

/* ctx is &g.fd: the bio needs the descriptor and nothing else, so there is no reason to
   drag in mbedtls_net_context for one integer. */
static int bio_send(void* ctx, const unsigned char* b, size_t n) {
  int fd = *(const int*)ctx;
  int r = (int)send(fd, b, n, 0);
  if (r < 0) {
    int e = errno; /* capture before any later call can replace it */
    if (e == EWOULDBLOCK || e == EAGAIN) return MBEDTLS_ERR_SSL_WANT_WRITE;
    g.sock_errno = e; /* retain the fatal send() reason for post-mortem diagnostics */
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  return r;
}
static int bio_recv(void* ctx, unsigned char* b, size_t n) {
  int fd = *(const int*)ctx;
  int r = (int)recv(fd, b, n, 0);
  if (r < 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN) return MBEDTLS_ERR_SSL_WANT_READ;
    g.sock_errno = errno; /* 104=RST 113=lwIP abort 128=ENOTCONN -- keep the reason */
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  /* 0 means the peer closed. "No data yet" is -1/EAGAIN and is handled above. Reporting a
     close as WANT_READ would hide it: the reader would spin until its whole budget expired
     and then treat whatever partial bytes arrived as a complete answer. A silent close is
     also the plausible shape of a decline. */
  if (r == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
  return r;
}

/* ---- role ---------------------------------------------------------------------------- */

int ad_sender_begin(const char* name) {
  if (ad_role_is_receiver()) return AD_SEND_E_ROLE;
  if (g.begun) return AD_SEND_E_OK;

  mbedtls_entropy_init(&g_ent);
  mbedtls_ctr_drbg_init(&g_drbg);
  mbedtls_ssl_config_init(&g_conf);
  mbedtls_ssl_init(&g_ssl);

  if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_ent, (const unsigned char*)"esp32drop-send", 14) != 0)
    return AD_SEND_E_NO_BEGIN;
  if (mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                  MBEDTLS_SSL_PRESET_DEFAULT) != 0)
    return AD_SEND_E_NO_BEGIN;
  /* We cannot verify the peer and do not pretend to: AirDrop's server certificate is issued
     under Apple's own CA, which this device has no copy of and no way to obtain. The
     receiving half of this library has run the same way since its first transfer. */
  mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_drbg);
  /* OFFER NO CERTIFICATE. macOS sends a CertificateRequest and then rejects a self-signed
     EC certificate with alert 46, certificate_unknown. A TLS 1.2 client with nothing
     suitable is not stuck: it sends an empty Certificate message, which is legal, and which
     macOS accepts. Configuring no own certificate is how mbedTLS does that. */
  if (mbedtls_ssl_setup(&g_ssl, &g_conf) != 0) return AD_SEND_E_NO_BEGIN;

  snprintf(g.sender_name, sizeof g.sender_name, "%s", (name && *name) ? name : "ESP32Drop");
  g.begun = true;
  g.state = AD_SEND_IDLE;
  return AD_SEND_E_OK;
}

void ad_peers_read(struct AdPeerTab* out) { awdl_peertab_read(out, millis(), AD_PEER_TTL_MS); }

/* ---- helpers ------------------------------------------------------------------------- */

static void close_conn(void) {
  if (g.tls_live) {
    mbedtls_ssl_close_notify(&g_ssl);
    g.tls_live = false;
  }
  if (g.fd >= 0) {
    close(g.fd);
    g.fd = -1;
  }
}

static void free_psram(void) {
  if (g.arch) {
    heap_caps_free(g.arch);
    g.arch = nullptr;
  }
  if (g.body) {
    heap_caps_free(g.body);
    g.body = nullptr;
  }
}

static void fail(int err) {
  // close_notify() can invoke bio_send() again and replace the original errno.
  const int original_sock_errno = g.sock_errno;
  close_conn();
  free_psram();
  g.sock_errno = original_sock_errno;
  /* KEEP THE STAGE THAT FAILED. state is about to become AD_SEND_FAILED, and with it the
     one fact that decides what a failure means: a connect timeout at DISCOVER says the peer
     was never listening, the same timeout at UPLOAD says it stopped listening while a human
     was deciding, and those call for opposite responses. It used to be discarded here. */
  if (g.state >= AD_SEND_DISCOVER && g.state <= AD_SEND_UPLOAD) g.failed_in = g.state;
  g.err = err;
  g.state = AD_SEND_FAILED;
  ad_cycle_hold(false);
}

static void make_xfer_id(void) {
  struct AwdlIdentity id;
  awdl_identity(&id);
  uint32_t r = (uint32_t)esp_random();
  /* 8-4-4-4-12. The last group is TWELVE hex digits: sharingd keys the session on this
     string and it has to parse as a UUID. Every proven transfer carried a 36-character
     value; a 32-character one is a different kind of silent failure from the same family
     as sending TransferID as a string instead of a dict. */
  snprintf(g.xfer_id, sizeof g.xfer_id, "%02X%02X%02X%02X-%04X-4%03X-8%03X-%08lX%04X", id.mac[0], id.mac[1], id.mac[2],
           id.mac[3], (unsigned)(r >> 16), (unsigned)(r & 0xfff), (unsigned)((r >> 4) & 0xfff),
           (unsigned long)esp_random(), (unsigned)(millis() & 0xffff));
}

/* Arm a stage. The socket is NOT created here: step_connect() makes it once any
   inter-stage gap has elapsed, so a deferral cannot leave a caller polling fd -1. */
static bool arm_stage(void) {
  g.fd = -1;
  g.step = AD_STEP_CONNECT;
  g.t_stage = g.t_prog = millis();
  return true;
}

/* Create the socket and start the non-blocking connect. */
static bool open_socket(void) {
  struct sockaddr_in6 a;
  memset(&a, 0, sizeof a);
  a.sin6_family = AF_INET6;
  a.sin6_port = htons(AD_SEND_PORT);
  uint8_t ll[16];
  awdl_ll_of_mac(g.peer_mac, ll);
  memcpy(&a.sin6_addr, ll, 16);
  a.sin6_scope_id = (uint32_t)awdl_netif_index();

  /* SEED ND6 FIRST. An outbound target has never sent us a unicast frame, so the neighbour
     cache holds nothing for it, lwIP falls back to real NDP, and AWDL never answers a
     neighbour solicitation -- the connect then sits behind an address that cannot resolve
     until it times out. The receive path gets this for free because a peer addresses US
     first. Per stage, not per send: the seed cache refreshes on a 500 ms timer and evicts
     round-robin from eight rows. */
  awdl_seed_peer(g.peer_mac);

  g.fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (g.fd < 0) return false;
  fcntl(g.fd, F_SETFL, fcntl(g.fd, F_GETFL, 0) | O_NONBLOCK);
  int r = connect(g.fd, (struct sockaddr*)&a, sizeof a);
  if (r < 0 && errno != EINPROGRESS) {
    g.sock_errno = errno;
    close(g.fd);
    g.fd = -1;
    return false;
  }
  return true;
}

/* One step of the non-blocking connect. 1 = done, 0 = pending, -1 = failed.
   Creates the socket on the first call at or after the inter-stage gap. */
static int step_connect(void) {
  if (g.fd < 0) {
    if (!open_socket()) {
      g.err = ADS_ERR_CONNECT_TIMEOUT;
      return -1;
    }
    g.t_stage = millis();
    return 0;
  }
  fd_set w;
  FD_ZERO(&w);
  FD_SET(g.fd, &w);
  struct timeval tv = {0, 0};
  int s = select(g.fd + 1, nullptr, &w, nullptr, &tv);
  if (s > 0) {
    int e = 0;
    socklen_t el = sizeof e;
    getsockopt(g.fd, SOL_SOCKET, SO_ERROR, &e, &el);
    if (e == 0) {
      /* THE SOCKET STAYS NON-BLOCKING. Making it blocking here would put mbedtls_ssl_read()
         inside a recv() with no timeout, and then step_response()'s budget is never
         consulted -- a budget you only reach between blocking calls is not a budget. In
         AD_SEND_ASK that means blocking until a human acts, or for ever if nobody does,
         which on loopTask is a watchdog panic rather than a timeout. bio_send/bio_recv
         already translate EAGAIN into WANT_WRITE/WANT_READ, which is exactly what the step
         functions expect. */
      mbedtls_ssl_session_reset(&g_ssl);
      mbedtls_ssl_set_bio(&g_ssl, &g.fd, bio_send, bio_recv, nullptr);
      g.tls_live = true;
      g.step = AD_STEP_TLS;
      return 1;
    }
    g.sock_errno = e;
    g.err = (e == ECONNREFUSED) ? ADS_ERR_CONNECT_REFUSED : ADS_ERR_CONNECT_TIMEOUT;
    return -1;
  }
  if (s < 0) {
    g.sock_errno = errno;
    g.err = ADS_ERR_CONNECT_TIMEOUT;
    return -1;
  }
  /* A connect that has not completed in 8 s is not going to: the peer's listener is open
     for a limited time after the wake advertisement stops, and waiting longer only spends
     window that the remaining stages need. */
  if ((uint32_t)(millis() - g.t_stage) > 8000) {
    g.sock_errno = 0; /* 0 distinguishes "never completed" from a real errno */
    g.err = ADS_ERR_CONNECT_TIMEOUT;
    return -1;
  }
  return 0;
}

/* How long a step may make NO progress before it is dead. See t_prog. */
#define AD_TLS_STALL_MS 15000u
/* 45 s, not the 10 s this was first given. That bound was reasoned from "a link whose
 * round trip is under a second" and never measured against a real upload -- and the first
 * real upload it met, it killed: the iPhone accepted, began receiving, and kept
 * waiting while the badge gave up and reported "stalled". The iPhone never noticed, because
 * nothing was wrong at its end.
 *
 * A write can legitimately make no progress for a long time here. The link has a 6.25 %
 * duty cycle, the peer's receive window closes and reopens, and ad_send_poll() issues at
 * most one 1 KB write per call -- so how often the sketch polls IS the transfer rate, and a
 * sketch that draws a screen can starve the write for many windows in a row. The number
 * that matters is not "how long since a byte moved" in the abstract; it is "long enough
 * that no plausible scheduling could explain it". 45 s is over forty of this link's
 * windows.
 *
 * ⚠️ STILL NOT MEASURED. It is a bound chosen to stop killing live transfers, not a
 * measurement of when a transfer is really dead. The stall report now carries sent/total
 * and the elapsed time so the next one says which. */
#define AD_REQ_STALL_MS 45000u
#define AD_HOLD_STALL_MS 20000u /* == AD_CYCLE_WATCH_MS: what the peer still allows */

static int step_tls(void) {
  int r = mbedtls_ssl_handshake(&g_ssl);
  if (r == 0) {
    g.step = AD_STEP_REQUEST;
    g.hdrs_off = 0;
    g.body_off = 0;
    g.chunk_hdr_sent = false;
    g.tail_sent = false;
    g.rsp_got = 0;
    g.rsp_end = 0; /* per STAGE, not just per attempt -- see step_response */
    g.t_stage = g.t_prog = millis();
    return 1;
  }
  if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
    /* A HANDSHAKE THAT NEVER FINISHES USED TO HANG HERE FOR EVER. WANT_READ is not an
       error, so this returned 0 and was polled again, and TCP has nothing to retransmit
       once the peer has simply stopped talking -- so lwIP never errored either. Measured
       handshakes on this link run 380 to 3,136 ms; 15 s is about five times the worst of
       them and well past anything the peer's listener life allows. */
    if ((uint32_t)(millis() - g.t_stage) > AD_TLS_STALL_MS) {
      g.tls_err = 0;
      g.err = ADS_ERR_STALLED;
      return -1;
    }
    return 0;
  }
  g.tls_err = r;
  g.err = ADS_ERR_TLS;
  return -1;
}

/* Write headers, then the single chunk, then the terminator. Resumable at every point. */
static int step_request(const uint8_t* bodyp, uint32_t bodylen) {
  /* Stall, not elapsed time. Every branch below that moves a byte stamps t_prog; if none
     has for AD_REQ_STALL_MS on a link whose round trip is under a second, the peer is
     gone. Without this, a /Upload write that stopped being accepted was bounded only by
     lwIP's retransmit exhaustion -- minutes, long past the window in which the peer would
     still have taken the file. */
  if ((uint32_t)(millis() - g.t_prog) > AD_REQ_STALL_MS) {
    /* Say WHERE it stopped. "stalled" alone cannot tell a transfer that never started from
       one that died with eight bytes left, and those are different bugs. */
    {
      char ln[96];
      snprintf(ln, sizeof ln, "SEND-STALL %lu/%lu B quiet_ms=%lu", (unsigned long)g.body_off, (unsigned long)bodylen,
               (unsigned long)(millis() - g.t_prog));
      awdl_diag_stage(ln, nullptr, 0);
    }
    g.err = ADS_ERR_STALLED;
    return -1;
  }
  if (g.hdrs_off < g.hdrs_len) {
    int r = mbedtls_ssl_write(&g_ssl, g.hdrs + g.hdrs_off, g.hdrs_len - g.hdrs_off);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    if (r <= 0) {
      g.tls_err = r;
      g.err = ADS_ERR_WRITE;
      return -1;
    }
    g.hdrs_off += (uint32_t)r;
    g.t_prog = millis();
    return 0;
  }
  if (g.body_off < bodylen) {
    uint32_t want = bodylen - g.body_off;
    if (want > 1024) want = 1024; /* one write, one segment's worth; MSS is 1220 */
    int r = mbedtls_ssl_write(&g_ssl, bodyp + g.body_off, want);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    if (r <= 0) {
      g.tls_err = r;
      g.err = ADS_ERR_WRITE;
      return -1;
    }
    g.body_off += (uint32_t)r;
    g.t_prog = millis();
    g.sent = g.body_off;
    return 0;
  }
  if (!g.tail_sent) {
    static const char tail[] = "\r\n0\r\n\r\n";
    int r = mbedtls_ssl_write(&g_ssl, (const unsigned char*)tail, 7);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    if (r <= 0) {
      g.tls_err = r;
      g.err = ADS_ERR_WRITE;
      return -1;
    }
    g.tail_sent = true;
    g.t_prog = millis();
    g.step = AD_STEP_RESPONSE;
    g.t_stage = millis();
    return 1;
  }
  return 1;
}

/* WHERE A RESPONSE ENDS -- parsed, not guessed.
 *
 * This used to test only for the chunked terminator "0\r\n\r\n", and that test cannot fire
 * on the reply that matters. The terminal 200 is Content-Length framed: reconstructed from
 * two recorded facts -- 62 bytes long, first 47 printable characters
 * "HTTP/1.1 200 OK..Connection: keep-alive..Conten" -- it is exactly
 *   "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n"
 * and no chunked shape lands on 62 bytes.
 *
 * The three-connection path only got away with it because the PEER CLOSED after every
 * response, so the read ended on EOF and never needed a terminator. Keep the connection
 * open and that exit vanishes: every reply would then run its whole budget, 60 s on /Ask
 * and 15 s on /Upload. /Discover is the one reply that genuinely is chunked (1,481 and
 * 1,846 bytes measured), which is why this stayed hidden.
 *
 * Returns the response's total length, or 0 while it is incomplete. */
static int rsp_complete_len(void) {
  int hend = -1;
  for (int i = 0; i + 3 < g.rsp_got; i++)
    if (memcmp(g.rsp + i, "\r\n\r\n", 4) == 0) {
      hend = i + 4;
      break;
    }
  if (hend < 0) return 0;

  bool chunked = false;
  long clen = -1;
  for (int i = 0; i < hend; i++) {
    if (clen < 0 && hend - i > 16 && strncasecmp((const char*)g.rsp + i, "Content-Length:", 15) == 0)
      clen = strtol((const char*)g.rsp + i + 15, nullptr, 10);
    if (!chunked && hend - i > 19 && strncasecmp((const char*)g.rsp + i, "Transfer-Encoding:", 18) == 0) {
      for (int j = i + 18; j < hend - 6; j++)
        if (strncasecmp((const char*)g.rsp + j, "chunked", 7) == 0) {
          chunked = true;
          break;
        }
    }
  }
  if (chunked) {
    /* Anchored past the headers, so a header byte sequence cannot spoof the terminator. */
    for (int i = hend; i + 5 <= g.rsp_got; i++)
      if (memcmp(g.rsp + i, "0\r\n\r\n", 5) == 0) return i + 5;
    return 0;
  }
  if (clen < 0) clen = 0; /* neither chunked nor a length: headers only */
  return (g.rsp_got >= hend + (int)clen) ? hend + (int)clen : 0;
}

/* One read. 1 = a complete response, 0 = still waiting, -1 = gave up.
 * budget_ms differs by stage: /Ask is waiting for a person. */
static int step_response(uint32_t budget_ms) {
  if (g.rsp_got < (int)sizeof g.rsp - 1) {
    int r = mbedtls_ssl_read(&g_ssl, g.rsp + g.rsp_got, sizeof g.rsp - 1 - (size_t)g.rsp_got);
    if (r > 0) {
      g.rsp_got += r;
      g.rsp[g.rsp_got] = 0;
      int n = rsp_complete_len();
      if (n) {
        g.rsp_end = n;
        return 1;
      }
      return 0;
    }
    if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE && r != MBEDTLS_ERR_SSL_TIMEOUT) {
      if (g.rsp_got > 0) return 1; /* peer closed after answering */
      /* THEY HUNG UP. This is not a timeout and it used to be reported as one, which sent
         anyone reading the screen looking for a peer that had gone to sleep -- when in
         fact the peer was awake enough to accept the connection, complete a handshake, and
         then close it with nothing in reply. Against a device whose /Discover had just
         succeeded, this happened in under three seconds where the /Ask budget is sixty:
         indistinguishable from "still waiting" in the old wording, and the opposite
         situation. mbedtls_ssl_read's code is carried so the two closes -- a clean TLS
         close_notify and a transport reset -- stay tellable apart. */
      g.tls_err = r;
      g.err = ADS_ERR_PEER_CLOSED;
      return -1;
    }
  }
  if (g.rsp_got >= (int)sizeof g.rsp - 1) {
    /* Filled before the response ended. Returning what we have would hand the next stage
       this reply's tail as its own head -- the remaining bytes live in mbedTLS, not in this
       buffer, so clearing rsp_got cannot undo it. Report it instead of truncating. */
    g.err = ADS_ERR_RESPONSE_TIMEOUT;
    return -1;
  }
  if ((uint32_t)(millis() - g.t_stage) > budget_ms) {
    /* A TIMEOUT IS A TIMEOUT. There used to be an "unless we already have a complete
       response" escape here -- if (g.rsp_got > 0 && g.rsp_end) return 1; -- and it could
       not ever be right. rsp_end becomes non-zero at exactly one place, four lines after
       rsp_complete_len() says the reply is whole, and that place returns 1 immediately and
       ends the stage. So within a stage the escape is unreachable; the only way to arrive
       here with rsp_end set is to have inherited it from the PREVIOUS stage, because
       rsp_end was reset per ATTEMPT and not per stage.
       What that produced: /Discover answers, leaving rsp_end at ~1846. /Ask then runs out
       its 60 s waiting for a human, having received some bytes but not a whole reply. The
       escape fires on the stale witness, rsp_status() reads a status line out of a reply
       that never finished arriving, and if those bytes happen to begin "HTTP/1.1 200" the
       machine advances to /Upload -- sending the file as though somebody had pressed
       Accept. A partial answer is not consent, and nothing else in this file can tell the
       difference afterwards. rsp_end is now also cleared per stage, below. */
    g.err = ADS_ERR_RESPONSE_TIMEOUT;
    return -1;
  }
  return 0;
}

static int rsp_status(void) {
  if (g.rsp_got < 12) return 0;
  if (memcmp(g.rsp, "HTTP/1.", 7) != 0) return 0;
  return atoi((const char*)g.rsp + 9);
}

/* ---- the three requests -------------------------------------------------------------- */

static bool build_discover(void) {
  struct AdBplistKV kv[] = {{"DeviceSupportFlags", AD_BP_INT, NULL, 0, 111611}};
  g.reqbody_len = ad_bplist_dict(g.reqbody, sizeof g.reqbody, kv, 1);
  if (!g.reqbody_len) return false;
  g.hdrs_len = (uint32_t)snprintf((char*)g.hdrs, sizeof g.hdrs,
                                  "POST /Discover HTTP/1.1\r\n"
                                  "User-Agent: AirDrop/1.0\r\n"
                                  "Connection: close\r\n"
                                  "Transfer-Encoding: chunked\r\n"
                                  "\r\n%X\r\n",
                                  (unsigned)g.reqbody_len);
  return true;
}

static bool build_ask(void) {
  static struct AdBpw w;
  ad_bpw_init(&w);
  int root = ad_bpw_dict(&w);

  /* TransferID is a DICT. Sending it as a string made macOS close the connection without
     answering -- the one-field difference between silence and a dialog. */
  int tid = ad_bpw_dict(&w);
  ad_bpw_set(&w, tid, "id", ad_bpw_str(&w, g.xfer_id));

  int ttype = ad_bpw_dict(&w);
  ad_bpw_set(&w, ttype, "files", ad_bpw_dict(&w)); /* an empty dict, as captured */

  int fdict = ad_bpw_dict(&w);
  ad_bpw_set(&w, fdict, "FileName", ad_bpw_str(&w, g.file.name));
  /* FileType is the ONLY place on the wire the type is spoken -- no HTTP header carries
     it, and cpio and DvZip never see it. ad_send() settled this value from the caller's
     MIME type or UTI, or from the leading bytes. public.png is the only value measured
     here; anything else goes out because the caller asked, and the peer's answer to /Ask
     is the verdict. */
  ad_bpw_set(&w, fdict, "FileType", ad_bpw_str(&w, g.file_uti));
  ad_bpw_set(&w, fdict, "FileSize", ad_bpw_int(&w, (int64_t)g.file.len));
  /* Cannot truncate: ad_send() bounded the name at AD_SEND_NAME_MAX. The 80 bytes this
     used to be cut a long name at 77 while FileName and the cpio entry went out whole,
     and the three have to agree. */
  char bom[AD_SEND_NAME_MAX + 4];
  snprintf(bom, sizeof bom, "./%s", g.file.name);
  ad_bpw_set(&w, fdict, "FileBomPath", ad_bpw_str(&w, bom));
  ad_bpw_set(&w, fdict, "FileIsDirectory", ad_bpw_bool(&w, false));
  ad_bpw_set(&w, fdict, "ShouldConvertMediaFormats", ad_bpw_bool(&w, false));

  int files = ad_bpw_array(&w);
  ad_bpw_push(&w, files, fdict);

  struct AwdlIdentity id;
  awdl_identity(&id);
  ad_bpw_set(&w, root, "TransferID", tid);
  ad_bpw_set(&w, root, "TransferType", ttype);
  ad_bpw_set(&w, root, "SenderID", ad_bpw_str(&w, id.instance));
  ad_bpw_set(&w, root, "BundleID", ad_bpw_str(&w, "com.apple.finder"));
  ad_bpw_set(&w, root, "SenderComputerName", ad_bpw_str(&w, g.sender_name));
  ad_bpw_set(&w, root, "SenderModelName", ad_bpw_str(&w, "ESP32"));
  ad_bpw_set(&w, root, "Items", ad_bpw_array(&w)); /* empty, as captured */
  ad_bpw_set(&w, root, "Files", files);
  ad_bpw_set(&w, root, "ConvertMediaFormats", ad_bpw_bool(&w, false));

  /* THE THREE KEYS A REAL macOS SENDER SENDS AND WE DO NOT.
   *
   * macOS as a RECEIVER accepts /Ask without any of them -- measured, repeatedly, files
   * arrive. An iPhone does not: it takes the request, shows no dialog, and answers nothing.
   * It is not a transport problem -- the connection lives
   * 47 s and no back-connection is ever attempted -- so what iOS rejects is the body.
   *
   * Bisected: an EMPTY SenderRecordData is the whole difference. FileIcon changes nothing
   * on either platform, and SenderIdentityAuthTag is still never sent -- its value sits
   * past the end of the capture, and transfers complete without it.
   *
   * ⚠️ A real SenderRecordData is Apple-signed identity material belonging to a person. It
   * is not replayed here, and it could not ship even if it worked. */
  /* ALWAYS, AND ALWAYS EMPTY. This one key is what lets an iPhone accept a transfer:
     without it iOS takes the request, shows nobody anything and answers nothing, while
     macOS accepts the very same body. With it -- zero bytes of it -- both accept.
     Measured: iPhone 0 -> 203 completed sends by adding this alone; Mac 147 completed with
     it, unchanged. FileIcon does not help either platform and is not sent.
     ⚠️ NOT anybody's identity. A real SenderRecordData is Apple-signed material belonging
     to a person; nothing of the sort is replayed here and none is needed. iOS is checking
     that the key is present, not what is in it. */
  ad_bpw_set(&w, root, "SenderRecordData", ad_bpw_data(&w, (const uint8_t*)"", 0));

  g.reqbody_len = ad_bpw_finish(&w, root, g.reqbody, sizeof g.reqbody);
  if (!g.reqbody_len) return false;
  g.hdrs_len = (uint32_t)snprintf((char*)g.hdrs, sizeof g.hdrs,
                                  "POST /Ask HTTP/1.1\r\n"
                                  "User-Agent: AirDrop/1.0\r\n"
                                  "Connection: keep-alive\r\n"
                                  "Transfer-Encoding: chunked\r\n"
                                  "\r\n%X\r\n",
                                  (unsigned)g.reqbody_len);
  return true;
}

static bool build_upload(void) {
  /* The two PSRAM blocks were taken in ad_send(), before /Discover -- see the note there.
     By here they exist or the send never started. */
  struct AdPackFile pf = {g.file.name, g.file.data, g.file.len, g.file.mtime};
  uint32_t n = ad_pack_cpio(g.arch, g.arch_len, &pf, 1);
  if (!n) {
    free_psram();
    return false;
  }
  /* Cut the archive into 128 KiB-grain records, like a real macOS sender does. A single
     record larger than the grain reaches sharingd whole but is rejected at 100% (the
     receiver's DvZip adapter assumes the grain); small files were one sub-grain record and
     always worked. Splitting is the fix. */
  uint32_t z = adz_dvzip_body(g.body, g.body_cap, g.arch, n, AD_DVZIP_GRAIN);
  if (!z) {
    free_psram();
    return false;
  }
  g.body_len = z;
  g.total = z;
  /* TotalBytes IS THE FILE SIZE. The captured sender's TotalBytes equalled its /Ask
     FileSize exactly. Putting the body length here makes the peer wait for bytes that never
     arrive: no reply, no close, just a stall. */
  g.hdrs_len = (uint32_t)snprintf((char*)g.hdrs, sizeof g.hdrs,
                                  "POST /Upload HTTP/1.1\r\n"
                                  "User-Agent: AirDrop/1.0\r\n"
                                  "TotalBytes: %lu\r\n"
                                  "Content-Type: application/x-dvzip\r\n"
                                  "TransferID: %s\r\n"
                                  "Connection: keep-alive\r\n"
                                  "Transfer-Encoding: chunked\r\n"
                                  "\r\n%X\r\n",
                                  (unsigned long)g.file.len, g.xfer_id, (unsigned)z);
  return true;
}

/* What a /Discover reply can and cannot tell us.
 *
 * ⚠️ THE bplist FIELDS ARE NOT PARSED, and the honest reason is that there is no reader for
 * them in this tree -- ad_bplist.h writes, it does not read. A substring scan was tried and
 * removed: in a real reply the keys are emitted first and the values after, so
 * "IsAirDropable" sits 1,300 to 1,636 bytes from its own boolean, on the far side of a
 * 1,296-byte ReceiverMediaCapabilities blob whose JSON bytes include 0x08 and 0x09. A scan
 * wide enough to reach the value is a scan that will find the wrong one.
 *
 * So airdropable reports what IS observable: the peer answered /Discover with 2xx. That is
 * the same verdict the hardware-proven path used -- it had no other gate and reached /Ask
 * on every run. IsAirDropable has never been observed false in this project, so nothing is
 * currently known to be lost; if a peer that answers 200 can still refuse, this will report
 * it as sendable and /Ask will be the one to say no.
 *
 * peer_name stays empty for the same reason. A caller wanting a cooldown key should use the
 * MAC and accept that it rotates about every 100 seconds -- which is what the GreetingCard
 * example does. */
static void parse_discover(void) {
  g.airdropable = (g.http_status >= 200 && g.http_status < 300);
  g.peer_name[0] = 0;
}

/* ---- the public verbs ---------------------------------------------------------------- */

int ad_send(const struct AdPeerRow* peer, const struct AdSendFile* files, int nfiles, unsigned flags) {
  if (!g.begun) return AD_SEND_E_NO_BEGIN;
  if (g.state != AD_SEND_IDLE && g.state != AD_SEND_DONE && g.state != AD_SEND_FAILED) return AD_SEND_E_BUSY;
  if (!peer || !peer->used || !files || nfiles != 1) return AD_SEND_E_ARG;
  if (!files[0].data || !files[0].len || !files[0].name) return AD_SEND_E_ARG;
  if (files[0].len > AD_SEND_LEN_MAX) return AD_SEND_E_ARG;
  {
    size_t nl = strlen(files[0].name);
    if (!nl || nl > AD_SEND_NAME_MAX || strchr(files[0].name, '/')) return AD_SEND_E_ARG;
  }

  /* WHAT THE FILE IS, settled by core/ad_uti.h from the caller's MIME type or UTI, or from
     the leading bytes when neither was given -- and never guessed. Bytes nobody can name, a
     MIME type with no UTI here, and a type the bytes provably contradict are all refused
     before a single millisecond of radio time is spent on them. This replaced a gate that
     accepted PNG and nothing else: that gate was not protecting the pipeline, which never
     sees a type at all, it was keeping a hardcoded "public.png" from lying. */
  const char* uti = nullptr;
  int how = 0;
  if (ad_uti_resolve(files[0].type, files[0].data, files[0].len, &uti, &how) != AD_UTI_OK) return AD_SEND_E_TYPE;
  if (awdl_netif_index() <= 0) return AD_SEND_E_NO_AWDL;

  memset(&g.peer_name, 0, sizeof g.peer_name);
  memcpy(g.peer_mac, peer->mac, 6);
  g.file = files[0];
  g.hold = (flags & AD_SEND_HOLD) != 0;
  g.err = ADS_OK;
  g.http_status = 0;
  g.tls_err = 0;
  g.airdropable = false;
  g.sent = 0;
  g.total = 0;
  g.rsp_got = 0;
  g.fd = -1;
  g.tls_live = false;
  g.sock_errno = 0;
  g.rsp_end = 0;
  g.failed_in = 0; /* per attempt: a value left from the last peer is a false witness */
  g.arch = g.body = nullptr;
  g.file_uti = uti;
  g.t_start = millis();
  make_xfer_id();

  {
    char ln[200];
    snprintf(ln, sizeof ln, "SEND-TYPE %s (%s) name=%s len=%lu\n", g.file_uti, ad_uti_how_name(how), g.file.name,
             (unsigned long)g.file.len);
    awdl_diag_stage(ln, nullptr, 0);
  }

  /* TAKE THE PSRAM HERE, BEFORE /Discover, so a file that cannot be built is refused on
     this side of the other person's screen. This used to happen in build_upload(), which
     runs after /Ask has returned 200 -- after a human pressed Accept. With 5 KB cards it
     never failed; with a multi-MB file on a fragmented part it is the first thing that
     would, and the failure would land after somebody had already said yes. Allocating
     rather than computing: two blocks have to fit, and only the allocator knows. */
  {
    struct AdPackFile pf = {g.file.name, g.file.data, g.file.len, g.file.mtime};
    g.arch_len = ad_pack_bound(&pf, 1);
    g.body_cap = adz_dvzip_body_bound(g.arch_len, AD_DVZIP_GRAIN) + 8;
    g.arch = (uint8_t*)heap_caps_malloc(g.arch_len, MALLOC_CAP_SPIRAM);
    g.body = (uint8_t*)heap_caps_malloc(g.body_cap, MALLOC_CAP_SPIRAM);
    if (!g.arch || !g.body) {
      free_psram();
      return AD_SEND_E_TOO_BIG;
    }
  }

  if (!build_discover()) return AD_SEND_E_ARG;
  if (!arm_stage()) {
    g.err = ADS_ERR_CONNECT_TIMEOUT;
    g.state = AD_SEND_FAILED;
    return AD_SEND_E_OK;
  }
  g.state = AD_SEND_DISCOVER;
  ad_cycle_hold(true); /* do not let the cycle pull AWDL out from under this */
  return AD_SEND_E_OK;
}

int ad_send_continue(void) {
  if (g.state != AD_SEND_HOLD_) return AD_SEND_E_STATE;
  if (!build_ask()) {
    fail(ADS_ERR_NO_MEM);
    return AD_SEND_E_OK;
  }
  if (!arm_stage()) {
    fail(ADS_ERR_CONNECT_TIMEOUT);
    return AD_SEND_E_OK;
  }
  g.state = AD_SEND_ASK;
  return AD_SEND_E_OK;
}

void ad_send_abort(void) {
  if (g.state == AD_SEND_IDLE || g.state == AD_SEND_DONE || g.state == AD_SEND_FAILED) return;
  fail(ADS_ERR_ABORTED);
}

void ad_send_poll(void) {
  if (!g.begun) return;
  switch (g.state) {
    case AD_SEND_DISCOVER:
    case AD_SEND_ASK:
    case AD_SEND_UPLOAD: {
      int r = 0;
      /* THE STEP BEFORE THE STEP RAN. Each step function advances g.step on success, so
         asking "are we in RESPONSE now" after calling one cannot tell "the response is
         complete" from "the request just finished and RESPONSE is next". Reading it
         afterwards made a sent request look like a received reply, and the reply was then
         parsed out of an empty buffer: err=HTTP_STATUS with http=0 and rsp=0B. */
      const int step_was = g.step;
      switch (g.step) {
        case AD_STEP_CONNECT:
          r = step_connect();
          break;
        case AD_STEP_TLS:
          r = step_tls();
          break;
        case AD_STEP_REQUEST:
          r = (g.state == AD_SEND_UPLOAD) ? step_request(g.body, g.body_len) : step_request(g.reqbody, g.reqbody_len);
          break;
        case AD_STEP_RESPONSE:
          /* /Ask is waiting for a person: 4 to 47 s measured, and never is normal. The
             other two are waiting for software. */
          r = step_response(g.state == AD_SEND_ASK ? 60000u : 15000u);
          break;
      }
      if (r < 0) {
        fail(g.err);
        return;
      }
      if (r == 0 || step_was != AD_STEP_RESPONSE) return;

      g.http_status = rsp_status();
      const int stage = g.state;
      close_conn(); /* each stage gets its own connection */

      if (stage == AD_SEND_DISCOVER) {
        if (g.http_status < 200 || g.http_status >= 300) {
          fail(ADS_ERR_HTTP_STATUS);
          return;
        }
        parse_discover();
        if (g.hold) {
          /* NO CONNECTION IS PARKED HERE. close_conn() ran a few lines above,
             unconditionally, and /Discover asks for Connection: close anyway -- an earlier
             comment claimed this held a live socket subject to the peer's idle timeout, and
             it does not. What IS at stake is the peer's LISTENER: it closes about 25 s
             after the wake advertisement stops on iOS, and the /Ask connect still has to
             land inside that. So the hold is bounded by the window it has to finish in,
             not by impatience with the caller. */
          g.state = AD_SEND_HOLD_;
          g.t_stage = millis();
          return;
        }
        if (!g.airdropable) {
          fail(ADS_ERR_NOT_AIRDROPABLE);
          return;
        }
        if (!build_ask()) {
          fail(ADS_ERR_NO_MEM);
          return;
        }
        if (!arm_stage()) {
          fail(ADS_ERR_CONNECT_TIMEOUT);
          return;
        }
        g.state = AD_SEND_ASK;
        return;
      }
      if (stage == AD_SEND_ASK) {
        /* 200 means the human accepted. Whether a decline arrives as a non-200 or as a
           silent close is NOT measured, so both land in the same two outcomes rather than
           being given a name they have not earned. */
        if (g.http_status != 200) {
          fail(ADS_ERR_HTTP_STATUS);
          return;
        }
        if (!build_upload()) {
          fail(ADS_ERR_NO_MEM);
          return;
        }
        if (!arm_stage()) {
          fail(ADS_ERR_CONNECT_TIMEOUT);
          return;
        }
        g.state = AD_SEND_UPLOAD;
        return;
      }
      /* UPLOAD */
      free_psram();
      if (g.http_status < 200 || g.http_status >= 300) {
        fail(ADS_ERR_HTTP_STATUS);
        return;
      }
      g.err = ADS_OK;
      g.state = AD_SEND_DONE;
      ad_cycle_hold(false);
      return;
    }

    case AD_SEND_HOLD_:
      /* BOUNDED, because it was not. A sketch that reaches HOLD and then neither continues
         nor aborts used to pin the machine here for ever -- and ad_cycle_hold(true) with
         it, so AWDL never came down, no further BLE wake happened, and the badge sat
         showing a peer it would never contact again. Twenty seconds is the watch window in
         ad_cycle.h: past it the /Ask connect would be refused anyway, so continuing is not
         a thing this can still do. An application that wants longer should let this expire
         and call ad_send() again after the next wake. */
      if ((uint32_t)(millis() - g.t_stage) > AD_HOLD_STALL_MS) {
        fail(ADS_ERR_STALLED);
        return;
      }
      return;

    default:
      return; /* IDLE / DONE / FAILED: nothing to advance */
  }
}

void ad_send_status_read(struct AdSendStatus* out) {
  memset(out, 0, sizeof *out);
  out->state = g.state;
  out->step = g.step;
  out->err = g.err;
  out->failed_in = g.failed_in;
  out->http_status = g.http_status;
  out->tls_err = g.tls_err;
  out->airdropable = g.airdropable;
  memcpy(out->peer_name, g.peer_name, sizeof out->peer_name);
  memcpy(out->peer_mac, g.peer_mac, 6);
  out->sent = g.sent;
  out->total = g.total;
  out->sock_errno = g.sock_errno;
  out->elapsed_ms = g.state == AD_SEND_IDLE ? 0 : (uint32_t)(millis() - g.t_start);
}

#endif /* ESP32DROP_SENDER */
