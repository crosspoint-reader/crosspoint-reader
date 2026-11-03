#include <freertos/idf_additions.h>

#include <algorithm>
/* ⚠️ THE RECEIVING HALF, AND IT IS ABSENT FROM A SENDER BUILD.
 *
 * Everything below compiles only when ESP32DROP_SENDER is NOT defined. The two roles cannot
 * share a device -- a listener task's 32,768-byte stack and a sender's lifelong TLS client
 * context do not both fit beside AWDL with anything left for an application -- so the build
 * decides which one exists instead of leaving a runtime flag to catch the mistake later.
 *
 * What a sender build stops carrying, measured: 5,662 bytes of static allocation
 * (g_adlog 1,616, ad_respond's frame 1,440, g_ssl 552, g_entropy 420, g_srvcert 408,
 * g_disc_body 320, g_ask_body 256, g_tlsconf 196, and the rest), plus every byte of the
 * receive path's code. More importantly it stops carrying ad_begin() itself: a send sketch
 * that called it by mistake would take a 32 KB task stack at runtime and only find out by
 * running short of memory somewhere else. Now it will not link, which is the earliest
 * possible place to be told.
 *
 * The symmetry is deliberate: ad_cycle.cpp and the bundled BLE advertiser are absent from a
 * RECEIVING build for exactly the same reason, in exactly the same way.
 */
#if !defined(ESP32DROP_SENDER)

/* ad_port_esp32.cpp -- the AirDrop application layer on the ESP32 backend.
 *
 * TLS termination, the HTTP/1.1 responder, the DvZip/cpio decode, and the handoff of a
 * finished file to the sketch. The protocol decisions and their reasoning travel with the code, because
 * every one of them was paid for with a capture.
 *
 * WHAT IS *NOT* HERE, and where it went:
 *   - the framing, de-chunking and poisoning rules  -> ../core/ad_http.h  (host-tested)
 *   - request dispatch and desync detection         -> ../core/ad_serve.h (host-tested)
 *   - the mDNS codec                                -> ../core/ad_dnssd.h (host-tested)
 * That split is not cosmetic: those three are where the bugs were, and they are the
 * three that can be run on a laptop against scripted bytes and scripted CLOCKS.
 *
 * THE ONE RULE ACROSS THE LAYER BOUNDARY: this file may include
 * exactly one thing from src/awdl/ -- the public ESP32AWDL.h. The toolchain does NOT
 * enforce that (measured: ../../awdl/core/*.h resolves perfectly well from here),
 * so it is enforced by grep in tools/check-library.sh instead.
 */
#include <Arduino.h> /* millis(), Serial -- the Serial uses leave at the strip */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>

#include "esp_heap_caps.h" /* heap_caps_malloc(..., MALLOC_CAP_SPIRAM): decode in PSRAM */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_cache.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

/* The ONE include from src/awdl, and the rule permits exactly this one: the public
   header. It brings awdl_ring.h, whose SPSC cursor the diagnostic ring below reuses
   rather than growing a second, untested copy. */
#include "../../ESP32AWDL.h"
#include "../core/ad_bplist.h"
#include "../core/ad_dvzip.h"
#include "../core/ad_http.h"
#include "../core/ad_serve.h"
#include "../core/airdrop_cert.h"
#include "ad_port_esp32.h"

/* gunzip via the ESP32-S3 mask-ROM miniz (tinfl). This is NOT an external package: the
 * function lives in the chip's ROM and is mapped by the vendor linker script
 * esp32s3.rom.ld (tinfl_decompress_mem_to_mem = 0x4000084c) shipped with the core.
 * Zero flash cost; decades-proven public-domain code baked into silicon. */
#define TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ((size_t)(-1))
extern "C" size_t tinfl_decompress_mem_to_mem(void* pOut_buf, size_t out_buf_len, const void* pSrc_buf,
                                              size_t src_buf_len, int flags);
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1 /* input is a zlib stream (78 xx ...), not raw */

// M3: a TCP listener on the AirDrop port (8770). If the iPhone opens the AirDrop
// /Discover connection to us on ch6, lwIP completes the 3-way handshake here and
// accept() fires -- the decisive proof that ch6 unicast connection is possible.
static volatile uint32_t g_tcp_accept = 0;
static volatile uint32_t g_tls_ok = 0;       // TLS handshake completed with a sender
static volatile uint32_t g_discover_ok = 0;  // answered HTTP POST /Discover (=> tile)
static volatile uint32_t g_ask_ok = 0;       // accepted a POST /Ask
static volatile uint32_t g_upload_ok = 0;    // received & decoded a POST /Upload image
static volatile uint32_t g_desync = 0;       // times the stream was out of frame

// Millisecond-stamped event log for the application layer. The symptom being
// chased ("the tile appears in the AirDrop window, then vanishes ~6s later") lives
// entirely in the few seconds after a successful /Discover, so every event in that
// window needs a timestamp on one greppable line. EV lines are meant to be read
// together offline, not individually.
// Formatted into a local buffer and emitted as a SINGLE write. Two tasks print to
// this port (frame processing and the TCP server), and Serial.printf is not atomic:
// out of 9153 captured lines exactly 2 were spliced together -- and one of those two
// swallowed the `http_idle_end ... why=timeout` line that was the sole evidence of a
// regression. A diagnostic channel that loses the one line that matters is worse than
// no channel, because it is trusted.

/* --- mDNS and connection counters ---------------------------------------------------
 * These were parked in AwdlDiag while the responder still lived in the link layer. They
 * count AirDrop events -- who asked, what we answered, which advertised port a sender
 * dialled -- so they belong here, with the code that writes them. */
static volatile uint32_t g_resp_tx = 0, g_resp_err = 0, g_query_seen = 0;
static volatile uint32_t g_notus = 0, g_ka_suppress = 0, g_dump_count = 0;
static volatile uint32_t g_tcp_tous = 0;
static volatile uint32_t g_syn_airdrop = 0, g_syn_pair1 = 0, g_syn_pair2 = 0, g_syn_other = 0;
static volatile uint16_t g_syn_other_port = 0;

/* ==========================================================================
 * mDNS: the advertisement, and the reply that has to leave from the frame path
 *
 * This is AirDrop POLICY -- which records we hold, which questions are ours, when a reply
 * is owed -- and it lives here rather than in the link layer. But it RUNS inside the AWDL
 * layer's receive tap, and that is not an accident of history: a query is answered at the
 * instant it is heard, because that is the only moment the querier is provably on channel
 * 6. Handing it to the netif queue instead can cost a full 1,048,576 us cycle.
 *
 * So the split is by ROLE rather than by layer. The codec is ad_dnssd.h, host-tested. The
 * policy is here. The timing -- inject now, un-gated -- is awdl_tx_ip6_now(), which the
 * link layer refuses to perform anywhere but inside this tap.
 * ========================================================================== */

/* The display name the two pairing services advertise. ONE definition: the encoder below
 * writes it into the PTR/SRV/TXT it emits, and ad_dnssd_known_answer_suppresses() compares
 * against it to decide whether a peer already holds that record. If those two ever
 * disagreed we would suppress a record we do not actually send, and go silently
 * undiscoverable -- the failure this whole area is built to avoid.
 *
 * ⚠️ It is a compile-time constant, and AirDrop.begin("name") cannot yet change it: what
 * the iPhone's share sheet actually DISPLAYS has never been measured, so nothing is
 * promised about it. */
#define AIRDROP_DNM "M5 Badge"

static struct AwdlIdentity g_id; /* filled once, at ad_begin() */

/* --- the two responses that carry our identity ------------------------------------
 *
 * The name on the sender's share sheet is ReceiverComputerName in the /Discover reply --
 * measured by giving three candidate name fields three different values and
 * looking at the tile. So the reply cannot be a constant: bplist strings are
 * length-prefixed, and a different name moves every offset in the table and three 64-bit
 * fields in the trailer. Both replies are built at ad_begin() and live here.
 *
 * ReceiverModelName picks the icon the sender draws [A -- not separately verified]. It
 * stays "M5Stack"; a user who wants a different one is choosing from Apple's set, not
 * from ours, and there is no measurement behind any other value. */
#define AD_NAME_MAX 64
static uint8_t g_disc_body[320], g_ask_body[256];
static uint32_t g_disc_len = 0, g_ask_len = 0;
static char g_name[AD_NAME_MAX] = {0};

static bool ad_build_identity(const char* name) {
  static const char MEDIA[] = "{\"Version\":1}";
  struct AdBplistKV disc[] = {
      {"ReceiverComputerName", AD_BP_STR, name, 0},
      {"ReceiverMediaCapabilities", AD_BP_DATA, MEDIA, (uint32_t)(sizeof(MEDIA) - 1)},
      {"ReceiverModelName", AD_BP_STR, "M5Stack", 0},
  };
  struct AdBplistKV ask[] = {
      {"ReceiverComputerName", AD_BP_STR, name, 0},
      {"ReceiverModelName", AD_BP_STR, "M5Stack", 0},
  };
  g_disc_len = ad_bplist_dict(g_disc_body, sizeof(g_disc_body), disc, 3);
  g_ask_len = ad_bplist_dict(g_ask_body, sizeof(g_ask_body), ask, 2);
  return g_disc_len && g_ask_len;
}

/* A byte search is not an interface. Nine lines, kept local rather than exported, the way
 * the link layer keeps its own copy. This is the FIRST, cheap filter: a message that
 * mentions neither the service nor our own label cannot possibly be for us, and parsing
 * every mDNS packet on a busy network from the frame path is not free. Anything that
 * survives it is then properly parsed by ad_dnssd.h, which is where the real decision is
 * made -- the substring test alone answered 15 foreign instance labels from 7 source MACs
 * with our full 1,107-byte response. */
static bool mem_contains(const uint8_t* p, int n, const char* sub, int sl) {
  if (sl <= 0 || n < sl) return false;
  for (int i = 0; i + sl <= n; i++)
    if (memcmp(p + i, sub, (size_t)sl) == 0) return true;
  return false;
}
static bool contains_airdrop(const uint8_t* p, int n) { return mem_contains(p, n, "_airdrop", 8); }

// Encode a dotted DNS name as length-prefixed labels ending in 0x00 (no
// compression — larger but unambiguous). Returns the advanced write pointer.
static uint8_t* put_dnsname(uint8_t* p, const char* name) {
  while (*name) {
    const char* dot = strchr(name, '.');
    int len = dot ? (int)(dot - name) : (int)strlen(name);
    *p++ = (uint8_t)len;
    memcpy(p, name, len);
    p += len;
    if (!dot) break;
    name = dot + 1;
  }
  *p++ = 0x00;
  return p;
}

// UDP checksum over the IPv6 pseudo-header (mandatory for IPv6).
static uint16_t udp6_checksum(const uint8_t* src, const uint8_t* dst, const uint8_t* udp, int udplen) {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i += 2) sum += (src[i] << 8) | src[i + 1];
  for (int i = 0; i < 16; i += 2) sum += (dst[i] << 8) | dst[i + 1];
  sum += (uint32_t)udplen;  // upper-layer length
  sum += 17;                // next header = UDP
  for (int i = 0; i + 1 < udplen; i += 2) sum += (udp[i] << 8) | udp[i + 1];
  if (udplen & 1) sum += udp[udplen - 1] << 8;
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  uint16_t ck = ~sum;
  return ck ? ck : 0xffff;
}

// Emit one modern-AirDrop "pairing" service (PTR + SRV + TXT), matching the shape
// a real Mac's sharingd advertises for _applicationServicePairing._tcp /
// _appSvcPrePair._tcp. On the current macOS/iOS stack sharingd carries the AirDrop
// identity (sn/at/sid/dnm) through these services and appears to require them (plus
// possibly a BLE pre-pair) before it will open the /Discover connection to a peer.
// This is the diagnostic experiment: if adding these makes sharingd send us a TCP
// SYN, the missing identity surface was the gate; if not, BLE pre-pair is mandatory.
// svc = e.g. "_applicationServicePairing._tcp.local"; host has the AAAA already.
static uint8_t* put_pair_service(uint8_t* p, const char* svc, uint16_t port, const char* host) {
  const char* DNM = AIRDROP_DNM;
  char inst[80];
  snprintf(inst, sizeof(inst), "%s.%s", DNM, svc);  // "M5 Badge._applicationServicePairing._tcp.local"
  uint8_t *rl, *rs;
  // PTR: svc -> inst (shared record, no cache-flush)
  p = put_dnsname(p, svc);
  *p++ = 0;
  *p++ = 12;
  *p++ = 0x00;
  *p++ = 0x01;  // PTR, class IN
  *p++ = 0;
  *p++ = 0;
  *p++ = 0x11;
  *p++ = 0x94;  // TTL 4500
  rl = p;
  p += 2;
  rs = p;
  p = put_dnsname(p, inst);
  {
    uint16_t n = p - rs;
    rl[0] = n >> 8;
    rl[1] = n;
  }
  // SRV: inst -> host:port
  p = put_dnsname(p, inst);
  *p++ = 0;
  *p++ = 33;
  *p++ = 0x80;
  *p++ = 0x01;  // SRV, class IN flush
  *p++ = 0;
  *p++ = 0;
  *p++ = 0x00;
  *p++ = 0x78;  // TTL 120
  rl = p;
  p += 2;
  rs = p;
  *p++ = 0;
  *p++ = 0;
  *p++ = 0;
  *p++ = 0;  // priority, weight
  *p++ = port >> 8;
  *p++ = port & 0xff;
  p = put_dnsname(p, host);
  {
    uint16_t n = p - rs;
    rl[0] = n >> 8;
    rl[1] = n;
  }
  // TXT: inst -> sn / at / sid / dnm
  p = put_dnsname(p, inst);
  *p++ = 0;
  *p++ = 16;
  *p++ = 0x80;
  *p++ = 0x01;  // TXT, class IN flush
  *p++ = 0;
  *p++ = 0;
  *p++ = 0x00;
  *p++ = 0x78;
  rl = p;
  p += 2;
  rs = p;
  char at[24], sid[64], dnm[24];
  snprintf(at, sizeof(at), "at=%s", g_id.instance);  // 12-hex identity tag
  snprintf(sid, sizeof(sid), "sid=%s", g_id.pair_sid);
  snprintf(dnm, sizeof(dnm), "dnm=%s", DNM);
  const char* kv[4] = {"sn=com.apple.sharingd.AirDrop", at, sid, dnm};
  for (int i = 0; i < 4; i++) {
    uint8_t l = (uint8_t)strlen(kv[i]);
    *p++ = l;
    memcpy(p, kv[i], l);
    p += l;
  }
  {
    uint16_t n = p - rs;
    rl[0] = n >> 8;
    rl[1] = n;
  }
  return p;
}

// Build the DNS message (mDNS authoritative answer advertising our service).
static int build_mdns_msg(uint8_t* d) {
  uint8_t* p = d;
  *p++ = 0;
  *p++ = 0;  // id
  *p++ = 0x84;
  *p++ = 0x00;  // flags: response + authoritative
  *p++ = 0;
  *p++ = 0;  // qdcount
  *p++ = 0;
  *p++ = 12;  // ancount = 12 (_airdrop: PTR,SRV,TXT,AAAA,2xNSEC
              //   + 2 pairing services x (PTR,SRV,TXT))
  *p++ = 0;
  *p++ = 0;  // nscount
  *p++ = 0;
  *p++ = 0;  // arcount
  char inst[48], host[24];
  snprintf(inst, sizeof(inst), "%s._airdrop._tcp.local", g_id.instance);
  snprintf(host, sizeof(host), "%s.local", g_id.instance);
  uint8_t* rl;
  uint8_t* rs;
  // PTR: _airdrop._tcp.local -> <inst>
  p = put_dnsname(p, "_airdrop._tcp.local");
  *p++ = 0;
  *p++ = 12;
  *p++ = 0x00;
  *p++ = 0x01;  // type PTR, class IN
  *p++ = 0;
  *p++ = 0;
  *p++ = 0x11;
  *p++ = 0x94;  // TTL 4500
  rl = p;
  p += 2;
  rs = p;
  p = put_dnsname(p, inst);
  {
    uint16_t n = p - rs;
    rl[0] = n >> 8;
    rl[1] = n;
  }
  // SRV: <inst> -> port + host
  p = put_dnsname(p, inst);
  *p++ = 0;
  *p++ = 33;
  *p++ = 0x80;
  *p++ = 0x01;  // type SRV, class IN flush
  *p++ = 0;
  *p++ = 0;
  *p++ = 0x00;
  *p++ = 0x78;  // TTL 120
  rl = p;
  p += 2;
  rs = p;
  *p++ = 0;
  *p++ = 0;
  *p++ = 0;
  *p++ = 0;  // priority, weight
  *p++ = AIRDROP_PORT >> 8;
  *p++ = AIRDROP_PORT & 0xff;  // port
  p = put_dnsname(p, host);
  {
    uint16_t n = p - rs;
    rl[0] = n >> 8;
    rl[1] = n;
  }
  // TXT
  p = put_dnsname(p, inst);
  *p++ = 0;
  *p++ = 16;
  *p++ = 0x80;
  *p++ = 0x01;
  *p++ = 0;
  *p++ = 0;
  *p++ = 0x00;
  *p++ = 0x78;
  rl = p;
  p += 2;
  rs = p;
  const char* txt = "flags=1019";
  uint8_t tl = strlen(txt);  // 0x3FB = macOS "Everyone" default bitmask (Discover bit set). The DvZip bit does not gate
                             // the /Upload format -- modern macOS/iOS send DvZip regardless -- so keep the validated
                             // value the sender's removeInvalidNodes accepts.
  *p++ = tl;
  memcpy(p, txt, tl);
  p += tl;
  {
    uint16_t n = p - rs;
    rl[0] = n >> 8;
    rl[1] = n;
  }
  // AAAA: host -> our link-local
  p = put_dnsname(p, host);
  *p++ = 0;
  *p++ = 28;
  *p++ = 0x80;
  *p++ = 0x01;
  *p++ = 0;
  *p++ = 0;
  *p++ = 0x00;
  *p++ = 0x78;
  *p++ = 0;
  *p++ = 16;
  memcpy(p, g_id.ll, 16);
  p += 16;
  // NSEC for host: declares "AAAA(28) exists, A does NOT" so iOS getaddrinfo
  // stops waiting for an A record and resolves on our IPv6. (Apple's own stack
  // always sends NSEC; without it the phone re-queries A forever, as observed.)
  p = put_dnsname(p, host);
  *p++ = 0;
  *p++ = 47;
  *p++ = 0x80;
  *p++ = 0x01;  // NSEC, class IN flush
  *p++ = 0;
  *p++ = 0;
  *p++ = 0x00;
  *p++ = 0x78;
  rl = p;
  p += 2;
  rs = p;
  p = put_dnsname(p, host);  // next-domain = self
  *p++ = 0x00;
  *p++ = 0x04;
  *p++ = 0x00;
  *p++ = 0x00;
  *p++ = 0x00;
  *p++ = 0x08;  // AAAA bit
  {
    uint16_t n = p - rs;
    rl[0] = n >> 8;
    rl[1] = n;
  }
  // NSEC for instance: declares SRV(33) + TXT(16) exist
  p = put_dnsname(p, inst);
  *p++ = 0;
  *p++ = 47;
  *p++ = 0x80;
  *p++ = 0x01;
  *p++ = 0;
  *p++ = 0;
  *p++ = 0x00;
  *p++ = 0x78;
  rl = p;
  p += 2;
  rs = p;
  p = put_dnsname(p, inst);
  *p++ = 0x00;
  *p++ = 0x05;
  *p++ = 0x00;
  *p++ = 0x00;
  *p++ = 0x80;
  *p++ = 0x00;
  *p++ = 0x40;
  {
    uint16_t n = p - rs;
    rl[0] = n >> 8;
    rl[1] = n;
  }
  // Modern-AirDrop identity surface: advertise the two pairing services so
  // sharingd (which queries these alongside _airdrop and carries its own identity
  // only through them) has an identity to promote us from "discovered" to a
  // /Discover connection. Both target our existing host (its AAAA is above).
  p = put_pair_service(p, "_applicationServicePairing._tcp.local", 54763, host);
  p = put_pair_service(p, "_appSvcPrePair._tcp.local", 54764, host);
  return p - d;
}

/* Build the complete IPv6 packet -- v6 header, UDP, and the DNS message -- addressed to
 * the mDNS multicast group. The link layer adds the 802.11 wrap.
 *
 * The packet is assembled at frame + AWDL_TX_HEADROOM so awdl_tx_ip6_now() can prepend
 * the awdl_data header in place, with no copy. Keeping the 802.11 wrap out of this file
 * is the layer rule rather than a convenience: it is what lets the mDNS responder live in
 * src/airdrop at all. */
static int build_mdns_ip6(uint8_t* frame) {
  uint8_t* buf = frame + AWDL_TX_HEADROOM;
  static const uint8_t mip[16] = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xfb};
  uint8_t dns[1280]; /* _airdrop(391) + 2 pairing services(~630) */
  int dlen = build_mdns_msg(dns);
  int udplen = 8 + dlen;

  uint8_t* p = buf;
  *p++ = 0x60;
  *p++ = 0x00;
  *p++ = 0x00;
  *p++ = 0x00;
  *p++ = (udplen >> 8) & 0xff;
  *p++ = udplen & 0xff;
  *p++ = 17;
  *p++ = 255; /* UDP, hop limit 255 */
  memcpy(p, g_id.ll, 16);
  p += 16;
  memcpy(p, mip, 16);
  p += 16;
  uint8_t* udp = p;
  *p++ = 0x14;
  *p++ = 0xe9;
  *p++ = 0x14;
  *p++ = 0xe9; /* sport/dport 5353 */
  *p++ = (udplen >> 8) & 0xff;
  *p++ = udplen & 0xff;
  *p++ = 0x00;
  *p++ = 0x00; /* checksum, filled below */
  memcpy(p, dns, dlen);
  p += dlen;
  uint16_t ck = udp6_checksum(g_id.ll, mip, udp, udplen);
  udp[6] = ck >> 8;
  udp[7] = ck & 0xff;
  return (int)(p - buf);
}

/* THERE IS NO PERIODIC MULTICAST ANNOUNCEMENT, and that is a decision with evidence
 * behind it (BRIEF_mdns_decision.md), not an omission. The reasoning sits here, beside the
 * responder that carries the same records, because a reader who wants one will look here.
 *
 * It could not be transmitted. esp_wifi_80211_tx refuses a DATA frame whose addr1 has the
 * group bit set -- disassembled out of ieee80211_raw_frame_sanity_check: a group address
 * always "hits" cnx_node_search, which routes the frame into the strict path, where an AP
 * interface then demands (fc1 & 3) == 2 and our frames are 0. A unicast destination MISSES
 * that lookup and returns success immediately, which is why the reply below works and this
 * did not. Setting FromDS to satisfy the check is not a fix: no real AWDL device emits
 * that shape, and it re-reads addr2 as the BSSID.
 *
 * It is also not load-bearing, which is what makes its absence a decision rather than a
 * hole:
 *   - Every record it carried, all twelve, is carried by the REPLY below -- ad_respond()
 *     does not look at the question and returns the same message.
 *   - Peers do ask. 387 decoded queries from the capture corpus: 186 for the _airdrop PTR,
 *     184 each for the two pairing service PTRs, 178 for our A record.
 *   - Across 35 separate runs in which mDNS was physically silent, 228 TCP connections
 *     were accepted and six uploads completed.
 *   - Discovery is carried by the AWDL service_response TLVs in every MIF action frame
 *     (awdl_frame.h), which do go out, 3.8 times a second.
 *
 * WHAT THIS GIVES UP, stated plainly: reaching a peer that has NOT asked. Nothing in the
 * record required that, but nothing in the record TESTED it either -- every capture is of
 * a device that had seen this badge before. A cold, never-paired device is the experiment
 * that could overturn this, and it is why this reasoning is written down rather than left
 * implicit in an absent function.
 *
 * The announce hook is deliberately kept: awdl_set_announce_cb() exists, the cadence fires
 * it in-window, and nothing registers. Its contract -- that awdl_tx_ip6_now() is legal for
 * the duration of the call -- is independently true and measured. What cannot be done is
 * the multicast destination, not the mechanism.
 */

/* The immediate reply, emitted on the frame task the instant a query is seen: one query
 * in, one reply out. */
static void ad_respond(const uint8_t querier[6]) {
  /* Its own buffer, never shared with the announcement's: the announcement runs on the
     cadence task at priority 2 and this runs on the frame task at 3, so the frame task
     can preempt a half-built announcement. That is why there were two static buffers
     before this code moved, and it is still why. */
  static uint8_t frame[AWDL_TX_HEADROOM + 1400];
  int n = build_mdns_ip6(frame);
  /* UNICAST to the querier, not multicast to ff02::fb, and that is measured rather than
   * preferred. esp_wifi_80211_tx refuses a DATA frame whose addr1 is a group address:
   * mDNS to 33:33:00:00:00:fb was rejected 100 % of the time, and the netif's own traffic
   * shows the same split (20 unicast accepted, 7 multicast refused in one 35-second
   * window). The case for multicast is that these queries are QM (class 0x0001, multicast
   * response expected) -- true, and moot for a frame that never leaves the driver. A
   * unicast reply the querier receives beats a QM-correct one it never sees. */
  if (awdl_tx_ip6_now(querier, frame, (size_t)n) == 0)
    g_resp_tx++;
  else
    g_resp_err++;
}

/* THE RX TAP. Runs on the AWDL frame task. Invariant: no Serial, no lwIP, no blocking --
 * awdl_diag_stage() and awdl_tx_ip6_now() are the only legal side effects.
 *
 * iplen is the length of the IPv6 packet, so dns_avail is iplen - 48 exactly (40 bytes of
 * v6 header plus 8 of UDP). That is what preserves the FCS fix across this seam: the
 * promiscuous callback's frame length carries the 4-byte 802.11 FCS, and taking the DNS
 * length from the UDP header instead was measured to be right in 156 of 156 dumps. */
static void ad_rx_tap(void*, const uint8_t src[6], const uint8_t* ip6, size_t iplen) {
  if (iplen < 48) return;
  uint8_t nh = ip6[6];
  if (nh == 17) {
    uint16_t sport = (ip6[40] << 8) | ip6[41], dport = (ip6[42] << 8) | ip6[43];
    if (dport != 5353 && sport != 5353) return;
    const uint8_t* dns = &ip6[48];
    int dns_avail = (int)iplen - 48;
    int udp_dlen = (int)((((uint16_t)ip6[44]) << 8) | ip6[45]) - 8;
    int dns_len = (udp_dlen >= 0 && udp_dlen <= dns_avail) ? udp_dlen : dns_avail;
    // Relevant = it mentions the _airdrop service OR our own instance/host id.
    // The phone's ADDRESS query "<id>.local A" does NOT contain "_airdrop", so
    // matching our instance id is essential to answer address resolution.
    bool relevant = contains_airdrop(dns, dns_len) || mem_contains(dns, dns_len, g_id.instance, strlen(g_id.instance));
    if (dns_len > 12 && relevant) {
      bool is_query = !(dns[2] & 0x80);
      int ka_dec = -1;  // -1 not a query, 0 replied, 1 known-answer suppressed, 2 not ours
      if (is_query) {
        g_query_seen++;
        // RFC 6762 7.1: do not answer a question whose answer the querier already
        // holds. We never honoured it, and it is not a nicety here -- a reply is 1,107
        // bytes built and injected from THIS task, measured 3,400-7,379us against a
        // 319us baseline. Measured over 101 captured queries, 23 carry every record we
        // would send. The decision is a pure function with 25 host tests
        // (tools/test-dnssd.sh), most of which assert it REFUSES to suppress: every way
        // it can be wrong ends with the badge quietly undiscoverable.
        // Three ways this ends, and only one of them transmits.
        //
        // The `relevant` test above is a raw substring match -- it passes any message
        // mentioning "_airdrop", including another device resolving ITS OWN instance.
        // Measured over 114 captured queries, 61 (54%) ask for nothing we hold and were
        // answered anyway, with the full 1,107-byte response. That is the largest single
        // source of the reply traffic whose cost is 3,400-7,379us of frame-path time.
        //
        // Both new tests are written to fail towards ANSWERING: a message that will not
        // parse, a name that will not resolve, a question count we cannot walk -- all of
        // them reply exactly as the old filter did. Only a query fully parsed, in which
        // no question names a record we hold, is dropped.
        struct AdDnssdOwn own = {g_id.instance, AIRDROP_DNM};
        if (!ad_dnssd_asks_us(dns, dns_len, &own)) {
          ka_dec = 2;
          g_notus++;
        } else if (ad_dnssd_known_answer_suppresses(dns, dns_len, &own)) {
          ka_dec = 1;
          g_ka_suppress++;
        } else {
          ka_dec = 0;
          ad_respond(src);
        }
      }
      // Dump the phone's (non-self) _airdrop DNS bytes for offline decode, so
      // we can see EXACTLY what it asks/announces. Capped to avoid flooding.
      // Spend the budget on what is RARE, not on what is common. A dropped reply
      // (ka_dec > 0) is the event worth having bytes for -- it is the decision that
      // could make us undiscoverable -- and it is rare by construction, so it bypasses
      // the rate limit entirely. Ordinary answered queries take the budget. Without
      // this the instrument captured 37 uninteresting queries and lost all four
      // interesting ones in the same window, which is the wrong way round.
      if (memcmp(src, g_id.mac, 6) != 0 && (ka_dec > 0 || awdl_diag_budget_ok())) {
        g_dump_count++;
        char hdr[96];
        // ka= carries the suppression decision for these exact bytes. The dump already
        // contains the query, so the decision and its input travel together and any
        // offline re-run of ad_dnssd_known_answer_suppresses() on the hex either agrees
        // or has found a wiring bug. Without it, "kasup=0" is indistinguishable from
        // "the function is never actually reached".
        snprintf(hdr, sizeof(hdr), "MDNS qr=%d src=%02x%02x%02x%02x%02x%02x len=%d ka=%d ", is_query ? 0 : 1, src[0],
                 src[1], src[2], src[3], src[4], src[5], dns_len, ka_dec);
        awdl_diag_stage(hdr, dns, dns_len);  // DUMP_MAX clamps; see stage_dump
      }
    }
  } else if (nh == 6) {
    const uint8_t* dst = &ip6[24];
    if (memcmp(dst, g_id.ll, 16) == 0) {  // to OUR address = iPhone connecting!
      g_tcp_tous++;
      uint8_t flags = iplen >= 54 ? ip6[40 + 13] : 0;  // TCP flags byte
      uint16_t dport = (ip6[42] << 8) | ip6[43];
      // Which of our ADVERTISED ports does the sender actually dial? We publish three
      // in mDNS -- _airdrop._tcp on 8770, _applicationServicePairing._tcp on 54763 and
      // _appSvcPrePair._tcp on 54764 -- but bind() only 8770, so a connection to
      // either pairing port gets a RST from lwIP. Earlier notes credited those pairing
      // records with the discovery-to-connection promotion, which may well be the
      // identity TXT they carry rather than the ports; this counts SYNs per port so
      // the question is answered by observation before anything is changed.
      if (flags & 0x02) {  // SYN
        if (dport == 8770)
          g_syn_airdrop++;
        else if (dport == 54763)
          g_syn_pair1++;
        else if (dport == 54764)
          g_syn_pair2++;
        else {
          g_syn_other++;
          g_syn_other_port = dport;
        }
      }
      // SNIFFER-ONLY, same reason as NS-TO-US above. sport is declared here rather than
      // with dport because this line is its only reader, so the receiver build does not
      // carry an unused local.
    }
  }
}

/* ---- staged diagnostics: the library does not own Serial ------------------------
 *
 * Every diagnostic line the AirDrop layer produces is now COPIED INTO A RING and written
 * out by whoever calls ad_diag_read_line() -- in practice the sketch's loop(). Three
 * reasons, in order of how much they cost when ignored:
 *
 * 1. A library must not decide that a user's Serial port is its log. There is no
 *    setLogSink() either: a sink is a callback, a callback runs on whatever task called
 *    it, and USBCDC::write busy-spins without yielding for up to 250 ms
 *    (cores/esp32/USBCDC.cpp) -- fifteen times one AW. Pull, never push.
 * 2. These lines were written from the TLS task. Serial.printf is not atomic, and this
 *    project has already lost the single line of evidence for a regression to two
 *    interleaved writes.
 * 3. Dropping is correct when the ring is full, and is counted. Blocking a protocol task
 *    to wait for a UART is how a diagnostic becomes an outage.
 *
 * SPSC, one producer (the TLS task) and one consumer, using the same cursor and the same
 * barrier discipline as the frame ring -- awdl_ring.h, which has 19 host checks. That is
 * the ONE thing this file takes from the AWDL layer, through the public header, which is
 * exactly what the layering rule in tools/check-library.sh permits.
 */
/* -DAD_DIAG_DETAIL turns on the instruments that answer OPEN QUESTIONS rather than report
 * the device's state: the TLS session-cache probes, the listener's stack high-water sample,
 * and the once-a-second listener-loop line. It is off by default because the ring below
 * holds eight slots and a per-second producer spends one of them every second -- the lines
 * a reader actually came for are the rare ones, and they are the ones a steady drip evicts.
 * Nothing outside this file's diagnostics changes with it: no frame is built differently,
 * no byte is parsed differently, and no counter reported through ad_stats_read() moves. */
#ifdef AIRDROP_CH_DUMP
#define AD_DIAG_SLOTS 64 /* the CH witness bursts ~50 lines per adoption */
#else
#define AD_DIAG_SLOTS 8
#endif
#define AD_DIAG_LINE 200
struct AdDiagRing {
  struct AwdlRing cur;
  char slot[AD_DIAG_SLOTS][AD_DIAG_LINE];
};
static struct AdDiagRing g_adlog;

static bool ad_diag_stage(const char* line, int n) {
  if (!g_adlog.cur.mask) return false; /* before ad_begin(): nowhere to put it */
  if (n <= 0) return false;
  if (n > AD_DIAG_LINE - 1) n = AD_DIAG_LINE - 1;
  int slot = awdl_ring_reserve(&g_adlog.cur);
  if (slot < 0) return false; /* counted in g_adlog.cur.drops */
  memcpy(g_adlog.slot[slot], line, (size_t)n);
  g_adlog.slot[slot][n] = 0;
  awdl_ring_commit(&g_adlog.cur);
  return true;
}

size_t ad_diag_read_line(char* dst, size_t cap) {
  int slot = awdl_ring_peek(&g_adlog.cur);
  if (slot < 0 || !cap) return 0;
  size_t n = strlen(g_adlog.slot[slot]);
  if (n > cap - 1) n = cap - 1;
  memcpy(dst, g_adlog.slot[slot], n);
  dst[n] = 0;
  awdl_ring_release(&g_adlog.cur);
  return n;
}

#ifdef AIRDROP_BODY_KEEP
/* Raw-body handoff for the probe build. Same release-flag discipline as g_arch_ready:
   the TLS task publishes g_body_ready LAST, loopTask reads it first. */
static uint8_t* g_body_block = nullptr;
static uint32_t g_body_len = 0;
static volatile bool g_body_ready = false;
#endif

#ifdef AIRDROP_BODY_KEEP
uint32_t ad_body_peek(const uint8_t** p) {
  if (!g_body_ready) return 0;  // acquire: read the flag before the pointer
  if (p) *p = g_body_block;
  return g_body_len;
}
void ad_body_release(void) {
  if (!g_body_ready) return;
  heap_caps_free(g_body_block);
  g_body_block = nullptr;
  g_body_len = 0;
  g_body_ready = false;
}
#endif

uint32_t ad_diag_dropped(void) { return g_adlog.cur.drops; }

/* Formats into a local buffer and stages it. The trailing newline stays in the format
   string exactly as it was when these were Serial.printf, so the output is unchanged. */
#define AD_LOG(fmt, ...)                                                            \
  do {                                                                              \
    char _l[AD_DIAG_LINE];                                                          \
    int _n = snprintf(_l, sizeof(_l), fmt, ##__VA_ARGS__);                          \
    if (_n > 0) ad_diag_stage(_l, _n < (int)sizeof(_l) ? _n : (int)sizeof(_l) - 1); \
  } while (0)

#define EV(fmt, ...) AD_LOG("EV %lu " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)
static uint32_t g_http_exit_ms = 0;   // when we last returned from serve_http
static uint32_t g_http_enter_ms = 0;  // when we last entered it
static uint32_t g_disc_ms = 0;        // when we last answered /Discover
static uint32_t g_ask_ms = 0;         // when we last answered /Ask (the /Upload clock)

// ---- received-image handoff (TLS task decodes -> main task draws) ------------
// All SPI/display I/O must stay on the main task; the TLS task only decodes into
// PSRAM and publishes the result. g_img_ready is set LAST as the release flag.
/* When the receive state last CHANGED. "Receiving" is a claim about now, and it used to be
 * set by the /Ask reply and cleared only by a completed upload -- so a sender that answered
 * /Ask and then never sent anything (observed: reconnected, finished TLS, then ten seconds
 * of silence and gone) left the badge saying "Receiving" indefinitely. It is the same
 * mistake as awdl_status_discoverable's, in a different layer: a state reported from an
 * event that happened rather than from one that is still happening. */
#define AD_RX_STALL_MS 20000u
static volatile uint32_t g_rx_touch_ms = 0;
static volatile bool g_rx_active = false;  // transfer in progress: "Receiving" UI
static volatile uint32_t g_rx_bytes = 0;   // compressed bytes received so far
// A finished transfer is a cpio ARCHIVE, not an image. The TLS task publishes the whole
// decoded block and does no parsing; ad_poll() walks it on the caller's task and hands over
// every regular file it holds. Parsing on the consumer side is deliberate: the walk is
// bounds-checked code fed by a remote sender (ad_cpio.h, 35 host checks under ASan), and
// there is no reason to run it inside the task that is holding the TLS session open.
static std::atomic<bool> g_arch_ready{false};  // an archive awaits delivery
static uint8_t* g_arch_block = nullptr;        // PSRAM block, freed after delivery
static const uint8_t* g_arch_buf = nullptr;    // the cpio within the block
static volatile uint32_t g_arch_len = 0;
static volatile uint32_t g_files_ok = 0;
static volatile uint32_t g_img_err = 0;  // undecodable/oversize transfers
static char g_img_errmsg[40] = {0};
// Accumulation budget for bodies that are NOT decoded in flight (gzip, bare cpio,
// and any shape the streaming sink declines): those still hold ~compressed +
// ~decompressed at once, so the old cap and the old reasoning stand for them.
// A DvZip body no longer pays this double occupancy -- it is decoded record by
// record while it arrives (struct RxPump below) and its ceiling is the PSRAM free
// block itself, minus the residue buffer. This constant also names the ceiling a
// HEALTHY device must be able to offer: when streaming cannot allocate even this
// much, the failure is reported as the device's (507), not the file's (413).
/* The archive ceiling. Was a hard 6 MiB; now whatever ad_begin() was given, because the
   sketch is the only thing that knows how much PSRAM it wants left for itself -- see the
   note on ad_begin() in the header. RX_MAX_AUTO is the bound used when the caller passes 0:
   still 6 MiB, so behaviour is unchanged for anyone who does not care. */
#define RX_MAX_AUTO (6u * 1024 * 1024)
static uint32_t g_rx_max = RX_MAX_AUTO;    /* policy, set by ad_begin() */
static volatile uint32_t g_rx_max_eff = 0; /* what a transfer actually got */

// ---- TLS server config, built once ------------------------------------------
// Reported in STAT even on the SNIFFER build (which has no mbedTLS at all), so
// these three live OUTSIDE the guard below.
static const char* g_tls_alg = "none";          // "EC"/"RSA" once cert loads
static int g_ec_crt_err = 0, g_ec_key_err = 0;  // why EC fell back
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_ctr_drbg;
static mbedtls_x509_crt g_srvcert;
static mbedtls_pk_context g_pkey;
static mbedtls_ssl_config g_tlsconf;
static mbedtls_ssl_context g_ssl;          // ONE session, reused via session_reset
static mbedtls_net_context g_net;          // .fd swapped to the active connection
static mbedtls_ssl_cache_context g_cache;  // abbreviated handshake on retries
static bool g_tls_ready = false;
static bool g_tls_tried = false;
static std::atomic<bool> g_stop{false}, g_listener_running{false};
enum class Consent { Idle, Asking, Accept, Decline };
static std::atomic<Consent> g_consent{Consent::Idle};
static uint8_t g_approved_peer[16];
static uint32_t g_approved_at = 0;
static bool g_approved = false;
static AdAskInfo g_request_info;
static portMUX_TYPE g_request_lock = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<uint32_t> g_limit_rejections{0};
static std::atomic<bool> g_stream_oversized{false};
void ad_reject_too_large() { g_stream_oversized.store(true); }
static uint32_t g_request_started_at = 0;
bool ad_request_info(AdAskInfo* out) {
  portENTER_CRITICAL(&g_request_lock);
  const bool pending = g_consent.load() == Consent::Asking;
  if (pending && out) *out = g_request_info;
  portEXIT_CRITICAL(&g_request_lock);
  return pending;
}
bool ad_request_pending() { return g_consent.load() == Consent::Asking; }
bool ad_request_answer(bool accept) {
  Consent pending = Consent::Asking;
  return g_consent.compare_exchange_strong(pending, accept ? Consent::Accept : Consent::Decline);
}

#ifdef AD_DIAG_DETAIL
// Probes around the session cache, to answer "why does resumption not fire on
// most reconnects" WITHOUT guessing. That is an open question, not a shipping
// feature, so it is behind AD_DIAG_DETAIL; with the define off mbedTLS gets its
// own cache callbacks directly and resumption behaves identically.
// The three distinguishable worlds:
//   - no `tlscache get` line before tlsok  -> the client offered no session ID
//     (mbedtls only consults the cache for a non-empty ID in the ClientHello)
//   - `get ... miss`                       -> client offered an ID we don't hold
//   - `get ... HIT` + small hs_ms          -> abbreviated handshake worked
// `set -> rc!=0` would mean the cache refused to store (e.g. alloc failure) --
// a path that is otherwise invisible, because mbedtls ignores f_set_cache's return.
static int tls_cache_get_probe(void* data, unsigned char const* id, size_t idlen, mbedtls_ssl_session* sess) {
  int r = mbedtls_ssl_cache_get(data, id, idlen, sess);
  EV("tlscache get id=%02x%02x%02x%02x len=%u -> %s", idlen > 0 ? id[0] : 0, idlen > 1 ? id[1] : 0,
     idlen > 2 ? id[2] : 0, idlen > 3 ? id[3] : 0, (unsigned)idlen, r == 0 ? "HIT" : "miss");
  return r;
}
static int tls_cache_set_probe(void* data, unsigned char const* id, size_t idlen, const mbedtls_ssl_session* sess) {
  int r = mbedtls_ssl_cache_set(data, id, idlen, sess);
  EV("tlscache set id=%02x%02x%02x%02x len=%u -> rc=%d", idlen > 0 ? id[0] : 0, idlen > 1 ? id[1] : 0,
     idlen > 2 ? id[2] : 0, idlen > 3 ? id[3] : 0, (unsigned)idlen, r);
  return r;
}
#endif /* AD_DIAG_DETAIL */

static void airdrop_tls_release() {
  mbedtls_ssl_free(&g_ssl);
  mbedtls_ssl_cache_free(&g_cache);
  mbedtls_ssl_config_free(&g_tlsconf);
  mbedtls_pk_free(&g_pkey);
  mbedtls_x509_crt_free(&g_srvcert);
  mbedtls_ctr_drbg_free(&g_ctr_drbg);
  mbedtls_entropy_free(&g_entropy);
  g_tls_ready = false;
  g_tls_tried = false;
}

static void airdrop_tls_init() {
  if (g_tls_ready) return;
  /* A RETRY MUST GIVE BACK WHAT THE LAST ATTEMPT TOOK.
   *
   * tcp_listen_task calls this once a second until it succeeds, and every call used to
   * re-init the same static contexts without freeing them. mbedtls_entropy_init allocates
   * a message-digest accumulator; re-initialising over it drops the pointer and leaks it.
   * A cert or key parsed by a pass that failed later leaks the same way.
   *
   * That made the retry a ONE-WAY RATCHET. Whatever caused the first failure -- a
   * transient shortage while the radio was busy, say -- the loop then ground internal RAM
   * down a few hundred bytes a second until even the accumulator would not fit, and from
   * then on every pass failed at seeding FOREVER, long after the original cause had
   * passed. Measured: 417 consecutive "[tls] seed -0x0034" lines over a 416-second
   * capture, with free internal heap pinned at 24 bytes and dipping to 0. The device was
   * not recovering because it could not: it was eating the memory it needed to recover.
   *
   * With the frees below, a pass that fails costs nothing, so a transient failure heals on
   * the next tick instead of becoming permanent. */
  if (g_tls_tried) airdrop_tls_release();
  g_tls_tried = true;
  mbedtls_entropy_init(&g_entropy);
  mbedtls_ctr_drbg_init(&g_ctr_drbg);
  mbedtls_x509_crt_init(&g_srvcert);
  mbedtls_pk_init(&g_pkey);
  mbedtls_ssl_config_init(&g_tlsconf);
  const char* pers = "ESP32Drop-tls";
  int r;
  psa_crypto_init();  // some EC/PK paths route through PSA (MBEDTLS_PSA_CRYPTO_C on)
  // EC P-256 cert: ECDSA sign is ~10-30ms vs RSA-2048's ~300ms. That speed is the
  // whole game here: the response must be generated inside the ~52ms ch6 window so
  // it reaches the sender before sharingd abandons the connection. (RSA sign blew
  // past the window -> reply landed a window late -> handshake never converged.)
  if ((r = mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func, &g_entropy, (const unsigned char*)pers,
                                 strlen(pers)))) {
    /* SAY HOW MUCH ROOM THERE WAS. "[tls] seed -0x0034" cost an hour: it names the failure
       and not the reason, and the reason here is almost always that this library's mbedTLS
       is built with CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC and internal RAM ran out at exactly
       this moment. Adding 2,088 bytes of static to the sketch was enough to do it, and the
       only visible symptom was that the device stopped appearing in the AirDrop share
       sheet -- with the radio, the election and the cadence all reporting healthy. */
    AD_LOG("[tls] seed failed -0x%04x, %lu B internal heap free (mbedTLS cannot use PSRAM)\n", -r,
           (unsigned long)esp_get_free_internal_heap_size());
    return;
  }
  int ec_ok = 0, rc, rk;
  rc = mbedtls_x509_crt_parse_der(&g_srvcert, AIRDROP_CRT_EC_DER, AIRDROP_CRT_EC_DER_LEN);
  rk = rc ? -1
          : mbedtls_pk_parse_key(&g_pkey, AIRDROP_KEY_EC_DER, AIRDROP_KEY_EC_DER_LEN, NULL, 0, mbedtls_ctr_drbg_random,
                                 &g_ctr_drbg);
  g_ec_crt_err = rc;
  g_ec_key_err = rk;
  if (!rc && !rk) {
    ec_ok = 1;
  } else
    AD_LOG("[tls] EC(DER) crt=-0x%04x key=-0x%04x -> RSA fallback\n", -rc, -rk);
  if (!ec_ok) {
    // Fallback so we always have a working server even if EC parse fails.
    mbedtls_x509_crt_free(&g_srvcert);
    mbedtls_x509_crt_init(&g_srvcert);
    mbedtls_pk_free(&g_pkey);
    mbedtls_pk_init(&g_pkey);
    if ((r = mbedtls_x509_crt_parse(&g_srvcert, (const unsigned char*)AIRDROP_CRT_RSA, strlen(AIRDROP_CRT_RSA) + 1))) {
      AD_LOG("[tls] RSA crt -0x%04x\n", -r);
      return;
    }
    if ((r = mbedtls_pk_parse_key(&g_pkey, (const unsigned char*)AIRDROP_KEY_RSA, strlen(AIRDROP_KEY_RSA) + 1, NULL, 0,
                                  mbedtls_ctr_drbg_random, &g_ctr_drbg))) {
      AD_LOG("[tls] RSA key -0x%04x\n", -r);
      return;
    }
  }
  if ((r = mbedtls_ssl_config_defaults(&g_tlsconf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT))) {
    AD_LOG("[tls] defaults -0x%04x\n", -r);
    return;
  }
  mbedtls_ssl_conf_authmode(&g_tlsconf, MBEDTLS_SSL_VERIFY_NONE);  // Everyone: no client cert
  mbedtls_ssl_conf_rng(&g_tlsconf, mbedtls_ctr_drbg_random, &g_ctr_drbg);
  mbedtls_ssl_conf_min_tls_version(&g_tlsconf, MBEDTLS_SSL_VERSION_TLS1_2);
  // Read timeout applies only to the HTTP phase (which uses the recv_timeout bio);
  // the TLS handshake runs non-blocking via select() and ignores this.
  mbedtls_ssl_conf_read_timeout(&g_tlsconf, 10000);
  // Session cache: sharingd's follow-up/retry connections resume abbreviated
  // (1-RTT, no cert, no EC sign) -- huge on this ~1-window/second link.
  mbedtls_ssl_cache_init(&g_cache);
  mbedtls_ssl_cache_set_max_entries(&g_cache, 4);
#ifdef AD_DIAG_DETAIL
  mbedtls_ssl_conf_session_cache(&g_tlsconf, &g_cache, tls_cache_get_probe, tls_cache_set_probe);
#else
  mbedtls_ssl_conf_session_cache(&g_tlsconf, &g_cache, mbedtls_ssl_cache_get, mbedtls_ssl_cache_set);
#endif
  if ((r = mbedtls_ssl_conf_own_cert(&g_tlsconf, &g_srvcert, &g_pkey))) {
    AD_LOG("[tls] own_cert -0x%04x\n", -r);
    return;
  }
  // Allocate the ONE ssl context now (34KB I/O buffers). session_reset() between
  // connections reuses these buffers -> no per-connection alloc churn/fragmentation.
  mbedtls_ssl_init(&g_ssl);
  if ((r = mbedtls_ssl_setup(&g_ssl, &g_tlsconf))) {
    AD_LOG("[tls] ssl_setup -0x%04x\n", -r);
    return;
  }
  g_tls_ready = true;
  g_tls_alg = mbedtls_pk_get_name(&g_pkey);
  AD_LOG("[tls] server config ready (%s, verify=none, select-loop)\n", g_tls_alg);
}

bool ad_prepare_tls() {
  if (g_listener_running.load()) return false;
  airdrop_tls_init();
  return g_tls_ready;
}

static int ssl_write_all(mbedtls_ssl_context* ssl, const unsigned char* p, int len) {
  int off = 0;
  while (off < len) {
    int r = mbedtls_ssl_write(ssl, p + off, len - off);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    if (r <= 0) return r;
    off += r;
  }
  return off;
}

// The TLS read timeout is a per-config value that mbedtls reads on every recv, so
// it can be retuned between phases of one session. One flat 10s value was very
// expensive: it is the price of every stall, and two different stalls were measured
// costing 10s each in a single /Discover exchange.
static void set_rd_timeout(uint32_t ms) { mbedtls_ssl_conf_read_timeout(&g_tlsconf, ms); }

// The byte source ad_http.h reads through. Its contract -- >0 bytes, 0 for a
// TIMEOUT, <0 for a close -- is exactly mbedtls's, and keeping the three distinct is
// the whole reason the layer can tell "nothing arrived yet" from "the peer is gone".
#ifdef HTTP_TRACE
/* ---- HTTP_TRACE: what a REAL Apple sender's /Discover and /Ask requests contain -------------
 *
 * The reference the send path's bplist writer is checked against. airdrop_cert.h's blobs
 * are RECEIVER-shaped, so a sender's request bodies are their own measurement,
 * and this is how that measurement is taken.
 *
 * IT SITS IN http_src_mbedtls(), on the one path every decrypted byte takes, so it cannot
 * go inert -- and it counts what it captured. That placement is what makes it worth
 * having: the /Ask -> /Upload transition is where desynchronisation hides, and the one
 * occurrence on record was caught only because a chunk-size line happened to look like a
 * hex number ("req 1F7BB"). A tap anywhere off the byte path cannot find that.
 *
 * DECRYPTED, unlike AIRDROP_CH_DUMP -- that one wraps the net BIO and therefore sees
 * ciphertext plus the plaintext ClientHello, which answers a different question.
 *
 * ⚠️ THE OUTPUT CONTAINS THE SENDER'S APPLE ID VALIDATION RECORD. It is capture of a
 * real person's identity material. It belongs in logs/ (which .gitignore excludes) and
 * MUST NOT be committed, and must not go into testdata/ even scrubbed.
 *
 * 4 KB per connection is deliberate: it covers /Discover, the /Ask body, the /Upload
 * request line and headers, and the first chunk header -- the entire region in question
 * -- without drowning the serial link in a 265 KB image. */
#ifndef HTTP_TRACE_MAX
#define HTTP_TRACE_MAX 4096
#endif
/* IT BUFFERS INTO PSRAM AND THE SKETCH DUMPS IT LATER, and that is not a refinement --
 * the first version staged hex lines directly and lost almost everything. Measured: of a
 * body the reader demonstrably consumed in full ("body read done ... to=0"), only 224
 * bytes reached the serial link. ht_put runs on the TLS task and can stage a hundred
 * lines in a millisecond; the ring is drained by loop() over USB CDC at ~45 B/ms. The
 * ring is drop-counted rather than blocking -- correctly, since a protocol task must not
 * wait on a log -- so the overflow is silent and looks exactly like a body that was never
 * read. AIRDROP_BODY_KEEP already solved this shape: hold the bytes, let the sketch pace
 * the output. Same pattern here. */
static uint8_t* g_ht_buf = NULL; /* PSRAM: mbedTLS may not have this memory anyway */
static int g_ht_len = 0;         /* bytes held                                     */
static int g_ht_off = 0;         /* cumulative decrypted-stream offset             */
static bool g_ht_full = false;

static void ht_arm(void) {
  if (g_ht_len > 0) return; /* a previous capture is still waiting to be read */
  if (!g_ht_buf) g_ht_buf = (uint8_t*)heap_caps_malloc(HTTP_TRACE_MAX, MALLOC_CAP_SPIRAM);
  g_ht_off = 0;
  g_ht_full = false;
}

static void ht_put(const unsigned char* b, int n) {
  if (!g_ht_buf || g_ht_full || n <= 0) return;
  int room = HTTP_TRACE_MAX - g_ht_off;
  if (n > room) {
    n = room;
    g_ht_full = true;
  }
  if (n > 0) {
    memcpy(g_ht_buf + g_ht_off, b, (size_t)n);
    g_ht_off += n;
    g_ht_len = g_ht_off;
  }
  if (g_ht_full) EV("httptrace full at %d B", g_ht_off);
}

/* Lend the capture to the sketch. The library still owns the block; ht_trace_release()
 * frees the hold so the next connection can arm. Same contract as ad_body_peek(). */
uint32_t ht_trace_peek(const uint8_t** p) {
  *p = g_ht_buf;
  return (uint32_t)g_ht_len;
}
void ht_trace_release(void) {
  g_ht_len = 0;
  g_ht_off = 0;
  g_ht_full = false;
}
#endif /* HTTP_TRACE */

static int http_src_mbedtls(struct HttpRdr* r, unsigned char* buf, int len) {
  mbedtls_ssl_context* ssl = (mbedtls_ssl_context*)r->ctx;
  int ret;
  do {
    ret = mbedtls_ssl_read(ssl, buf, (size_t)len);
  } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
  if (ret == MBEDTLS_ERR_SSL_TIMEOUT) return 0;
  if (ret <= 0) return -1;
#ifdef HTTP_TRACE
  ht_put(buf, ret);
#endif
  return ret;
}
static void http_set_timeout(struct HttpRdr* r, uint32_t ms) {
  r->timeout_ms = ms;
  set_rd_timeout(ms);
}

// Firmware adapters for the transport-agnostic dispatch layer (ad_serve.h).
// serve() below fills an HttpServe with these; the same struct on the host test
// side points write/now/log at a capture buffer and a scripted clock.
static int sv_write(struct HttpServe* s, const unsigned char* p, int n) {
  return ssl_write_all((mbedtls_ssl_context*)s->io, p, n);
}
static uint32_t sv_now(struct HttpServe*) { return millis(); }
static void sv_log(struct HttpServe*, const char* l) { EV("%s", l); }

static void set_nonblock(int fd, bool nb) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) return;
  fcntl(fd, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

// One borrowed chunk crosses from TLS to ad_poll. The producer retains its
// buffer until the consumer releases it, including timeout/cancellation races.
enum class StreamSlot { Idle, Pending, Processing };
static std::atomic<StreamSlot> g_stream_slot{StreamSlot::Idle};
static ad_stream_cb_t g_stream_cb = nullptr;
static void* g_stream_ctx = nullptr;
static AdStreamEvent g_stream_event;
static const uint8_t* g_stream_data = nullptr;
static uint32_t g_stream_len = 0;
static bool g_stream_result = false;
void ad_on_stream(ad_stream_cb_t cb, void* ctx) {
  g_stream_cb = cb;
  g_stream_ctx = ctx;
}
static bool stream_deliver(AdStreamEvent event, const uint8_t* data = nullptr, uint32_t len = 0) {
  if (g_stop.load()) return false;
  g_stream_event = event;
  g_stream_data = data;
  g_stream_len = len;
  g_stream_result = false;
  g_stream_slot.store(StreamSlot::Pending);
  const uint32_t started = millis();
  bool cancelled = false;
  while (g_stream_slot.load() != StreamSlot::Idle) {
    if (g_stop.load() || millis() - started >= 10000) {
      cancelled = true;
      auto pending = StreamSlot::Pending;
      if (g_stream_slot.compare_exchange_strong(pending, StreamSlot::Idle)) break;
      // A running callback owns the bytes until it returns; never free under it.
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return !cancelled && g_stream_result;
}

// ---- /Upload receive + decode ----------------------------------------------
// Chunked and Content-Length bodies use the same DvZip record pump. With a
// stream callback, fixed scratch is handed to ad_poll for incremental storage.
// Without it, the legacy API retains a complete decoded archive in PSRAM.
// The TLS task never touches storage or the display. Receipt failures precede
// the response; success waits for the consumer's final validation.

enum {
  RXM_UNDECIDED = 0,  // fewer than 10 body bytes: no shape yet
  RXM_STREAM,         // DvZip signature seen: decode records as they complete
  RXM_RAW
};  // anything else (gzip, bare cpio): accumulate, as always

enum {
  RXF_NONE = 0,
  RXF_OOM,  // this device could not give the memory -> 507
  RXF_CAP,  // the body outgrew what this device can ever hold -> 413
  RXF_INFLATE
};  // a record's zlib would not inflate -> 400

struct RxPump {
  uint8_t* out;  // whole archive or fixed record scratch, owned by RxPump until release
  bool streaming;
  uint32_t out_len, out_cap;
  uint32_t in_total;      // body bytes ever appended -- sink->len is only the residue
  uint32_t residue_prev;  // sink->len as this hook last left it, for the delta
  uint32_t nrec, nstored;
  int end;             // AdDvzipEnd of the FINAL drain; init to END_RUNNING
  uint8_t mode;        // RXM_*
  uint8_t fail;        // RXF_*, sticky
  bool psram_limited;  // out_cap was capped by free PSRAM, not by any policy:
                       // turns a later CAP into 507 (device) instead of 413 (file)
};

/* Decode ONE complete record into the output block. Shared by the in-flight drain
   and the whole-buffer fallback below, so there is exactly one decoder.
   NO PER-RECORD LOGGING on success: the first version of the decode logged every
   record, an eighteen-line burst into an eight-slot ring, and the lines it evicted
   were the summary and the cut report -- the two it existed to produce. */
static bool rx_pump_emit(void* vp, const struct AdDvzipRec* rec, const uint8_t* payload) {
  struct RxPump* p = (struct RxPump*)vp;
  uint32_t room = p->streaming ? 256u * 1024u : p->out_cap - p->out_len;
  uint8_t* target = p->out + (p->streaming ? 0 : p->out_len);
  if (rec->stored) {
    /* A STORED RECORD IS A COPY, NOT A DECOMPRESSION. macOS switches to these once
       deflate stops paying on an already-compressed image -- measured, 6 of the 18
       records in a 2.35 MB photo. Feeding one to the inflater is what used to cut the
       stream and deliver half an archive. */
    if (rec->len > room) {
      p->fail = p->streaming ? RXF_INFLATE : RXF_CAP;
      AD_LOG("DVZIP-CAP: stored record %lu wants %lu B, %lu B room\n", (unsigned long)(p->nrec + 1),
             (unsigned long)rec->len, (unsigned long)room);
      return false;
    }
    memcpy(target, payload, rec->len);
    if (p->streaming &&
        (rec->len > p->out_cap - p->out_len || !stream_deliver(AdStreamEvent::Data, target, rec->len))) {
      p->fail = rec->len > p->out_cap - p->out_len ? RXF_CAP : (g_stream_oversized.load() ? RXF_CAP : RXF_OOM);
      return false;
    }
    p->out_len += rec->len;
    p->nrec++;
    p->nstored++;
    return true;
  }
  size_t got = tinfl_decompress_mem_to_mem(target, room, payload, rec->len, TINFL_FLAG_PARSE_ZLIB_HEADER);
  if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
    /* mem_to_mem reports one bit: failed. WHICH failure decides the HTTP status --
       out of room is this device's problem, a broken zlib stream is the sender's --
       so split them by room: the measured record grain is 128 KiB uncompressed, and
       a conforming record given two grains of room cannot have run out of space. */
    p->fail = (room < 2u * AD_DVZIP_GRAIN) ? RXF_CAP : RXF_INFLATE;
    AD_LOG("DVZIP-REC: record %lu (%lu B zlib) failed with %lu B room -> %s\n", (unsigned long)(p->nrec + 1),
           (unsigned long)rec->len, (unsigned long)room, p->fail == RXF_CAP ? "cap" : "inflate");
    return false;
  }
  if (p->streaming && (got > p->out_cap - p->out_len || !stream_deliver(AdStreamEvent::Data, target, got))) {
    p->fail = got > p->out_cap - p->out_len ? RXF_CAP : (g_stream_oversized.load() ? RXF_CAP : RXF_OOM);
    return false;
  }
  p->out_len += (uint32_t)got;
  p->nrec++;
  return true;
}

/* Adopt streaming only on the DvZip wire signature: a plausible first record length
   and a zlib header at offset 4 (or bit 31 -- a stored first record, never yet seen
   first but legal). gzip's 1f 8b and cpio's "07070" both read as absurd record
   lengths and fall to RXM_RAW, where the sink accumulates exactly as it always has.
   A genuine DvZip body that fails this test (first record over the sane cap) also
   falls to RAW and is STILL decoded -- once, at the end, through the same pump -- so
   the streaming test gates only WHEN decoding happens, never WHETHER. */
#define RX_REC_SANE (1u * 1024 * 1024) /* largest measured compressed payload: 131,118 B */
static int rx_body_shape(const uint8_t* b, uint32_t n) {
  if (n < 10) return RXM_UNDECIDED; /* same floor as ad_dvzip_looks_like */
  uint32_t hdr = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
  uint32_t rl = ad_dvzip_reclen(hdr);
  if (rl == 0 || rl > RX_REC_SANE) return RXM_RAW;
  if (ad_dvzip_is_flagged(hdr)) return RXM_STREAM;
  if (b[4] != 0x78 || ((((uint32_t)b[4] << 8) | b[5]) % 31) != 0) return RXM_RAW;
  return RXM_STREAM;
}

/* The output block, allocated ONCE, all-or-nothing, the moment streaming is adopted.
 *
 * Upfront rather than grown, because growth is what failed before: a doubling realloc
 * of a multi-MB block needs old and new at once when it cannot extend in place, and
 * 4 MiB + 7 MiB does not fit in an 8 MiB part -- the same arithmetic that killed the
 * 2,352,506-byte transfer this redesign started from. One allocation also moves the
 * OOM to record one, where it is answered as 507 before megabytes have flown, instead
 * of at second 70 of a 71-second transfer. The slack is given back by a shrinking
 * realloc at the end, the move the receive path already used.
 *
 * RX_OUT_HEADROOM is what the transfer itself may still need beside the output: the
 * residue buffer is already allocated when this runs, so this covers only incidental
 * PSRAM use during the transfer. Ceiling arithmetic, measured base: 7,864,308 B
 * largest free block - 320 KiB residue - this headroom ~= 7.1 MB of decoded archive. */
static const uint32_t RX_OUT_HEADROOM = 384u * 1024;

static bool rx_pump_open(struct RxPump* p) {
  if (g_stream_cb) {
    p->streaming = true;
    p->out_cap = g_rx_max;
    g_rx_max_eff = g_rx_max;
    // Port owns one fixed PSRAM scratch block; rx_upload frees it on every exit.
    p->out = (uint8_t*)heap_caps_malloc(256u * 1024u, MALLOC_CAP_SPIRAM);
    if (!p->out || !stream_deliver(AdStreamEvent::Begin)) {
      p->fail = RXF_OOM;
      return false;
    }
    AD_LOG("RX-SD: fixed 256 KiB decode scratch, %lu B archive ceiling\n", (unsigned long)p->out_cap);
    return true;
  }
  /* TAKE WHAT THE SKETCH ALLOWED, NOT WHAT PSRAM HAPPENS TO HAVE.
     This used to ask for largest_free_block - headroom, i.e. nearly all of PSRAM, for the
     length of every transfer. That is right for a badge that does nothing else and wrong
     for a sketch with its own buffers -- and the failure it produces is the sketch's
     allocation returning NULL, which is a worse place for it to land than here. So the
     ceiling is g_rx_max, and PSRAM only lowers it. */
  uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  uint32_t avail = (largest > RX_OUT_HEADROOM) ? largest - RX_OUT_HEADROOM : 0;
  uint32_t want = (g_rx_max < avail) ? g_rx_max : avail;
  p->psram_limited = avail < g_rx_max;
  g_rx_max_eff = want;
  p->out = want ? (uint8_t*)heap_caps_malloc(want, MALLOC_CAP_SPIRAM) : nullptr;
  if (!p->out) {
    AD_LOG("RX-OOM: decode block of %lu B refused, %lu free / %lu largest in PSRAM\n", (unsigned long)want,
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    p->fail = RXF_OOM;
    return false;
  }
  p->out_cap = want;
  AD_LOG("RX-STREAM: DvZip signature, decode-while-receive, %lu B ceiling%s\n", (unsigned long)want,
         p->psram_limited ? " (PSRAM-limited)" : "");
  return true;
}

// PSRAM-backed sink for ad_http.h's chunked receiver.
/* Publishes progress as it happens. Called after every append, so the counter on the badge
   moves during a transfer instead of jumping from 0 to the total at the end. */
/* Set when a grow failed for want of memory, cleared at the start of each upload. It is
   the difference between 507 and 413 to the sender, and between "buy a smaller phone" and
   "this file is over the limit" to the user. */
static bool g_rx_grow_failed = false;

/* Called after every successful append; in RXM_STREAM it is also the decoder (the
   consuming-hook contract in ad_http.h). g_rx_bytes publishes TOTAL body bytes,
   which sink->len no longer is once records are being consumed out of it. */
static void rx_sink_progress(struct HttpSink* sk) {
  struct RxPump* p = (struct RxPump*)sk->ctx;
  p->in_total += sk->len - p->residue_prev;
  g_rx_bytes = p->in_total;
  g_rx_touch_ms = (uint32_t)(esp_timer_get_time() / 1000);
  if (p->mode == RXM_UNDECIDED && !p->fail) {
    p->mode = (uint8_t)rx_body_shape(sk->buf, sk->len);
    if (p->mode == RXM_STREAM && !rx_pump_open(p)) p->mode = RXM_RAW; /* fail=RXF_OOM set */
  }
  if (p->mode == RXM_STREAM && !p->fail) {
    /* EVERY BUFFER HERE IS FED BY A REMOTE STRANGER'S DEVICE. The output block bounds
       decoded bytes, but once records are consumed the accumulate cap (sink->max) never
       sees them, so INPUT needs its own bound or a stream of records decoding to nothing
       keeps this sink receiving for ever. Output tracks input to +0.09% measured, so
       input a megabyte past the output ceiling is not a photo. */
    if (p->in_total > p->out_cap + (1u << 20)) {
      p->fail = RXF_CAP;
      AD_LOG("RX-CAP: %lu B in vs %lu B out ceiling -- input bound tripped\n", (unsigned long)p->in_total,
             (unsigned long)p->out_cap);
    }
  }
  if (p->mode == RXM_STREAM && !p->fail) {
    int end = AD_DVZIP_END_RUNNING;
    uint32_t consumed = ad_dvzip_drain(sk->buf, sk->len, false, rx_pump_emit, p, &end);
    if (consumed) {
      memmove(sk->buf, sk->buf + consumed, sk->len - consumed);
      sk->len -= consumed;
    }
  }
  if (p->fail) {
    /* Stop the receive at the next chunk header rather than draining megabytes we can
       no longer store: max=0 turns any further chunk into HTTP_RECV_OVERFLOW, and
       rx_upload translates the reason into a status BEFORE the response -- which is
       the entire point of decoding in flight. */
    sk->max = 0;
    if (p->fail == RXF_OOM || (p->fail == RXF_CAP && p->psram_limited)) g_rx_grow_failed = true; /* -> 507, not 413 */
  }
  p->residue_prev = sk->len;
}

static bool rx_sink_grow(struct HttpSink* sk, uint32_t need) {
  if (g_stream_cb && need > 512u * 1024u) return false;
  uint32_t nc = sk->cap ? sk->cap : (512u * 1024);
  while (nc < need) nc <<= 1;
  if (g_stream_cb) nc = std::min<uint32_t>(nc, 512u * 1024u);
  uint8_t* np = (uint8_t*)heap_caps_realloc(sk->buf, nc, MALLOC_CAP_SPIRAM);
  if (!np) {
    /* SAY WHICH FAILURE THIS IS. A failed grow returns false, http_recv_chunked turns that
       into HTTP_RECV_OVERFLOW, and the caller reports "file too large" -- which names a
       policy limit when what actually happened is that PSRAM ran out. The two want
       opposite responses from a user, and they used to be indistinguishable in the log.
       This changes no behaviour; it only stops the log from misdiagnosing. */
    g_rx_grow_failed = true;
    AD_LOG("RX-OOM: grow %lu -> %lu B failed (need %lu), %lu free / %lu largest in PSRAM\n", (unsigned long)sk->cap,
           (unsigned long)nc, (unsigned long)need, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    return false;
  }
  sk->buf = np;
  sk->cap = nc;
  return true;
}

// Receive the /Upload body, decoding a DvZip stream WHILE it arrives, so the
// outcome is known BEFORE the response is written -- receive+decode, judge, answer.
// Called on the TLS task, reading through the connection's persistent reader `r`.
// Sets g_upload_ok / g_img_* on success, g_img_err on failure.
static std::atomic<bool> g_upload_pending{false};
bool ad_upload_pending() { return g_upload_pending.load(); }

static void rx_upload(struct HttpRdr* r, bool chunked, int clen) {
  mbedtls_ssl_context* ssl = (mbedtls_ssl_context*)r->ctx;
  g_stream_oversized.store(false);
  g_upload_pending.store(true);
  g_rx_active = true;
  g_rx_bytes = 0;
  g_rx_touch_ms = (uint32_t)(esp_timer_get_time() / 1000);
  g_rx_grow_failed = false;
  uint32_t err0 = g_img_err;
  bool response_sent = false;

  struct RxPump pump;
  memset(&pump, 0, sizeof(pump));
  pump.end = AD_DVZIP_END_RUNNING; /* zero is END_RECORD, which claims certainty */
#ifdef AIRDROP_BODY_KEEP
  pump.mode = RXM_RAW; /* the probe build keeps the RAW body: decoding
                          in flight would destroy the evidence it exists
                          to capture */
#endif

  /* The receive buffer. Under streaming it holds only the RESIDUE -- one held-back
     record plus one arriving one, worst legit case 131,122 + ~131,126 B at the
     measured grain -- so 320 KiB never has to grow. A RAW body (gzip, bare cpio)
     accumulates whole and grows on demand exactly as before. */
  uint32_t cap = 320u * 1024, len = 0;
  uint32_t body_total = 0;
  uint8_t* gz = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  bool overflow = false, clean_end = false;
  if (!gz) {
    strlcpy(g_img_errmsg, "out of PSRAM", sizeof(g_img_errmsg));
    g_img_err++;
    goto done;
  }

  // The de-chunker, the DvZip completion probe and the poisoning rules all live in
  // ad_http.h, compiled unchanged into tools/test-http.sh. There is exactly one
  // implementation: a second copy here is how a latent bug survived being flashed
  // (the settle branch used to append a chunk without re-judging completeness, so a
  // DvZip end record arriving right after a boundary probe was consumed and ignored;
  // the host tests found it).
  if (chunked) {
    struct HttpSink sk;
    memset(&sk, 0, sizeof(sk));
    sk.buf = gz;
    sk.cap = cap;
    sk.len = 0;
    sk.max = g_stream_cb ? 512u * 1024u : g_rx_max;
    sk.grow = rx_sink_grow;
    sk.progress = rx_sink_progress;
    sk.ctx = &pump; /* the progress hook decodes through this */
    // 10s while the body is flowing; 3000ms of silence to accept a boundary-aligned
    // DvZip stream. 3000ms because measured stalls on this link are 3004ms and
    // 4104ms and a shorter settle would answer 200 OK on a truncated body whose
    // DvZip prefix still decompresses -- a corrupt image reported as a success.
    int rc = http_recv_chunked(r, &sk, 10000, 3000, http_set_timeout);
    gz = sk.buf;
    cap = sk.cap;
    len = sk.len; /* len: whole body (RAW) or residue (STREAM) */
    body_total = (pump.mode == RXM_STREAM) ? pump.in_total : len;
    g_rx_bytes = body_total;
    switch (rc) {
      case HTTP_RECV_OK_END_RECORD:
        clean_end = true;
        AD_LOG("UPLOAD: complete via DvZip end record\n");
        break;
      case HTTP_RECV_OK_LAST_CHUNK:
        clean_end = true;
        AD_LOG("UPLOAD: complete via HTTP last-chunk\n");
        break;
      case HTTP_RECV_OK_SILENCE:
        clean_end = true;
        AD_LOG("UPLOAD: complete via boundary + 3s silence\n");
        break;
      case HTTP_RECV_OVERFLOW:
        overflow = true;
        break;
      default:
        /* The reason was thrown away here, which is why two identical 2,352,506-byte
           failures could not be told apart. rc IS the reason. */
        AD_LOG("UPLOAD-STOP: rc=%d after %lu bytes\n", rc, (unsigned long)body_total);
        break;
    }
    EV("upload_recv rc=%d total=%lu residue=%lu decoded=%lu poisoned=%d eof=%d", rc, (unsigned long)body_total,
       (unsigned long)len, (unsigned long)pump.out_len, r->poisoned, r->eof);
  } else if (clen > 0) {
    HttpSink sk{};
    sk.buf = gz;
    sk.cap = cap;
    sk.max = g_stream_cb ? 512u * 1024u : g_rx_max;
    sk.ctx = &pump;
    uint32_t remaining = clen;
    if (remaining > g_rx_max + (1u << 20)) {
      pump.fail = RXF_CAP;
      overflow = true;
    }
    while (remaining && !overflow && !g_stop.load()) {
      uint32_t n = std::min<uint32_t>(4096, remaining);
      if (n > sk.max - sk.len || (sk.len + n > sk.cap && !rx_sink_grow(&sk, sk.len + n))) {
        overflow = true;
        break;
      }
      int got = hr_read(r, sk.buf + sk.len, n);
      if (got <= 0) break;
      sk.len += got;
      remaining -= got;
      rx_sink_progress(&sk);
      if (pump.fail) overflow = true;
      if (got != static_cast<int>(n) || r->poisoned) break;
    }
    gz = sk.buf;
    cap = sk.cap;
    len = sk.len;
    body_total = pump.in_total;
    clean_end = remaining == 0;
  }

  /* STREAMED BODIES FINISH DECODING HERE, BEFORE THE RESPONSE. The residue is at
     most one held-back record (plus the end record's four bytes), so this is one
     tinfl call against the ~1s window sharingd allows a response -- not the
     whole-body decode that used to sit, unreportable, after the 200. */
  if (pump.mode == RXM_STREAM && !pump.fail && len) {
    int dend = AD_DVZIP_END_RUNNING;
    (void)ad_dvzip_drain(gz, len, true, rx_pump_emit, &pump, &dend);
    pump.end = dend;
  }
  /* A pump failure IS a receipt failure: fold it into the same ladder. (The 507/413
     split repeats here because the final drain runs outside the progress hook.) */
  if (pump.fail == RXF_OOM || pump.fail == RXF_CAP) {
    overflow = true;
    if (pump.fail == RXF_OOM || pump.psram_limited) g_rx_grow_failed = true;
  }

  {  // Reject known transport failures now; success waits for the SD callback.
    const char* status = "200 OK";
    /* INFLATE is judged first: aborting a receive mid-body (the sink's max=0 clamp)
       surfaces as OVERFLOW too, and the record that would not inflate is the truer
       fact than the abort it caused. */
    if (pump.fail == RXF_INFLATE)
      status = "400 Bad Request";
    else if (overflow)
      status = g_rx_grow_failed ? "507 Insufficient Storage" : "413 Content Too Large";
    else if (body_total < 6)
      status = "400 Bad Request";
    char h[128];
    int hl = snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
    if (status[0] != '2') {
      ssl_write_all(ssl, (const unsigned char*)h, hl);
      response_sent = true;
      /* DO NOT CLOSE ON TOP OF THE STATUS WE JUST WROTE.
       *
       * Writing a non-2xx and returning immediately closes the connection with megabytes
       * of body still unread, and in lwIP a close with unread data is tcp_abort -- a RST.
       * A RST discards our OWN unsent send queue, and on a link with a 6.25% duty cycle
       * the status is still sitting in that queue waiting for a 65 ms window. So the
       * failure report we just composed would be destroyed by the close that follows it,
       * and the sender would see the same silence as before. This tree already knows the
       * mechanism: it is written out at ad_serve.h as RFC 2525 2.17, and it is why the
       * /Ask drain exists.
       *
       * BEST-EFFORT, and honestly so. Bounded at 3 s, which is enough to carry the status
       * out through two windows and to avoid the immediate RST. It is NOT enough to
       * guarantee the sender reads it: today's /Ask measurement showed sharingd posts its
       * receive only once its whole send is queued, so with more than its 131,072-byte
       * send buffer still unsent it may never look. Draining that much could take minutes
       * at the 27 KB/s this link gives, which is not a trade worth making blind.
       * UPLOAD-REPLY-DRAIN is logged so a field observation can settle whether the Mac
       * actually shows the failure; until it does, this is a fix for one known destroyer
       * of the message, not a proof the message arrives. */
      uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
      uint32_t binned = 0;
      uint32_t now;
      http_set_timeout(r, 300);
      while (!r->eof && !g_stop.load() && ((now = (uint32_t)(esp_timer_get_time() / 1000)) - t0) < 3000) {
        int g = hr_fill(r);
        if (g > 0) {
          binned += (uint32_t)g;
          r->consumed += g;
          r->p = r->n;
        }
      }
      AD_LOG("UPLOAD-REPLY: %s; drained %lu B in %lu ms before closing\n", status, (unsigned long)binned,
             (unsigned long)((uint32_t)(esp_timer_get_time() / 1000) - t0));
    }
  }
  AD_LOG("UPLOAD: received %lu bytes (chunk=%d ovf=%d complete=%d)\n", (unsigned long)body_total, chunked, overflow,
         clean_end);
  if (pump.fail == RXF_INFLATE) { /* before overflow, same order as the status */
    snprintf(g_img_errmsg, sizeof(g_img_errmsg), "record %lu would not inflate", (unsigned long)(pump.nrec + 1));
    g_img_err++;
    goto done;
  }
  if (overflow) {
    strlcpy(g_img_errmsg, g_rx_grow_failed ? "out of PSRAM" : "file too large", sizeof(g_img_errmsg));
    g_img_err++;
    if (pump.fail == RXF_CAP && !pump.psram_limited) g_limit_rejections.fetch_add(1);
    goto done;
  }
  if (body_total < 6) {
    strlcpy(g_img_errmsg, "empty body", sizeof(g_img_errmsg));
    g_img_err++;
    goto done;
  }

  if (!clean_end) {
    strlcpy(g_img_errmsg, "incomplete upload", sizeof(g_img_errmsg));
    g_img_err++;
    goto done;
  }

  if (g_stream_cb) {
    if (pump.streaming && ad_dvzip_end_ok(pump.end) && stream_deliver(AdStreamEvent::Finish)) {
      g_upload_ok++;
      AD_LOG("UPLOAD-SD-OK: %lu decoded bytes\n", (unsigned long)pump.out_len);
    } else {
      g_img_err++;
      strlcpy(g_img_errmsg, "stream incomplete or storage failed", sizeof(g_img_errmsg));
    }
    goto done;
  }

  if (pump.mode != RXM_STREAM) {
    // dump the body head so the format is unambiguous next run (1f8b=gzip, 070707=odc
    // cpio, else=unknown/DvZip). Streamed bodies consumed their head long ago; their
    // shape is already on the RX-STREAM line.
    char hx[3 * 32 + 1] = {0};
    int hn = (len < 32) ? (int)len : 32;
    for (int i = 0; i < hn; i++) sprintf(hx + i * 3, "%02x ", gz[i]);
    AD_LOG("UPLOAD: body head (%lu bytes): %s\n", (unsigned long)len, hx);
  }

#ifdef AIRDROP_BODY_KEEP
  /* THE PROBE BUILD KEEPS THE BODY INSTEAD OF PARSING IT.
   *
   * Every question we have asked about this stream so far -- is the top bit of a record
   * length a flag, is the payload stored or deflated, does the record grain stay 128 KiB --
   * has cost one physical AirDrop of a 2.4 MB photo and about 71 s of a person's time, and has
   * answered exactly one hypothesis. That trade is bad and it does not have to be made:
   * the bytes themselves answer every hypothesis, including the ones nobody has thought of
   * yet, and they answer them offline, for ever.
   *
   * So this build does not inflate. It hands the raw body to the sketch, which writes it
   * out and lets tools/decode-body.py reconstruct it to a file. One AirDrop, then the
   * question moves to a host test where it belongs.
   *
   * Skipping the inflate is not laziness -- it is what makes the peak safe. Parsing would
   * hold the 2.35 MB body and a 2.65 MB output buffer at once; this holds only the body. */
  if (!g_body_ready) {
    g_body_len = len;
    g_body_block = gz;
    gz = nullptr;
    g_body_ready = true;  // release flag: set LAST
    AD_LOG("BODY-KEEP: %lu bytes held for dump\n", (unsigned long)g_body_len);
  } else {
    AD_LOG("BODY-KEEP: previous body not yet dumped, discarding %lu bytes\n", (unsigned long)len);
  }
  goto done;
#endif

  {
    const uint8_t* cpio = nullptr;
    uint32_t cpio_len = 0;
    uint8_t* outbuf = nullptr;
    if (pump.mode == RXM_STREAM) {
      // Decoded already, during receipt -- the outcome the response above was built
      // from. All that is left is to give back the slack: the output block was
      // allocated at the ceiling, and a shrinking realloc to the real size frees the
      // tail in place.
      heap_caps_free(gz);
      gz = nullptr;  // the residue served its purpose
      if (pump.out_len) {
        uint8_t* shr = (uint8_t*)heap_caps_realloc(pump.out, pump.out_len, MALLOC_CAP_SPIRAM);
        if (shr) pump.out = shr;
      }
      outbuf = pump.out;
      pump.out = nullptr;  // ownership moves to the handoff
      cpio = outbuf;
      cpio_len = pump.out_len;
      AD_LOG("DVZIP: %lu records (%lu stored), %lu in -> %lu out, end=%s\n", (unsigned long)pump.nrec,
             (unsigned long)pump.nstored, (unsigned long)body_total, (unsigned long)cpio_len,
             ad_dvzip_end_name(pump.end));
      /* A WALK THAT STOPPED IS NOT A WALK THAT FINISHED. The whole files inside a
         short archive are still real files, so they are still delivered -- but the
         archive is not called complete. */
      if (!ad_dvzip_end_ok(pump.end)) {
        snprintf(g_img_errmsg, sizeof(g_img_errmsg), "dvzip %s at %lu of %lu", ad_dvzip_end_name(pump.end),
                 (unsigned long)cpio_len, (unsigned long)body_total);
        g_img_err++;
      }
    } else if (gz[0] == 0x1f && gz[1] == 0x8b && gz[2] == 0x08) {  // gzip -> gunzip
      uint32_t off = 10;
      uint8_t flg = gz[3];
      if (flg & 0x04) {
        if (off + 2 <= len) {
          off += 2 + (gz[off] | (gz[off + 1] << 8));
        }
      }  // FEXTRA
      if (flg & 0x08) {
        while (off < len && gz[off]) off++;
        off++;
      }  // FNAME
      if (flg & 0x10) {
        while (off < len && gz[off]) off++;
        off++;
      }  // FCOMMENT
      if (flg & 0x02) {
        off += 2;
      }  // FHCRC
      uint32_t isize =
          (len >= 4) ? (gz[len - 4] | (gz[len - 3] << 8) | (gz[len - 2] << 16) | ((uint32_t)gz[len - 1] << 24)) : 0;
      if (off + 8 > len || isize == 0 || isize > g_rx_max) {
        strlcpy(g_img_errmsg, "bad gzip", sizeof(g_img_errmsg));
        g_img_err++;
        goto done;
      }
      outbuf = (uint8_t*)heap_caps_malloc(isize, MALLOC_CAP_SPIRAM);
      if (!outbuf) {
        strlcpy(g_img_errmsg, "out of PSRAM", sizeof(g_img_errmsg));
        g_img_err++;
        goto done;
      }
      size_t out = tinfl_decompress_mem_to_mem(outbuf, isize, gz + off, len - off, 0);
      if (out == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
        heap_caps_free(outbuf);
        strlcpy(g_img_errmsg, "inflate failed", sizeof(g_img_errmsg));
        g_img_err++;
        goto done;
      }
      cpio = outbuf;
      cpio_len = out;
      heap_caps_free(gz);
      gz = nullptr;                            // free compressed ASAP
    } else if (memcmp(gz, "07070", 5) == 0) {  // already-raw cpio (odc/newc)
      cpio = gz;
      cpio_len = len;
    } else {
      // A DvZip body that was NOT streamed: the probe build (BODY_KEEP forces RAW), a
      // non-chunked body (the progress hook never ran), or a first record the streaming
      // signature declined. Decoded here, once, through the SAME pump -- one decoder,
      // whichever moment it runs at. This is the one decode still on the wrong side of
      // the response; every shape a current macOS/iOS sender actually produces takes
      // the streamed path above.
      bool dvzip = ad_dvzip_looks_like(gz, len);
      /* GIVE THE SLACK BACK FIRST. The receive buffer grows by DOUBLING, so the body
         may sit in a block nearly twice its size while the pump below is about to ask
         for the output ceiling out of an 8 MiB part. Shrinking to the actual length
         costs one realloc that almost always stays in place. */
      {
        uint8_t* shr = (uint8_t*)heap_caps_realloc(gz, len ? len : 1, MALLOC_CAP_SPIRAM);
        if (shr) gz = shr;
      }
      if (dvzip && rx_pump_open(&pump)) {
        int dvend = AD_DVZIP_END_RUNNING;
        (void)ad_dvzip_drain(gz, len, true, rx_pump_emit, &pump, &dvend);
        pump.end = dvend;
        heap_caps_free(gz);
        gz = nullptr;
        if (pump.fail) {
          heap_caps_free(pump.out);
          pump.out = nullptr;
          strlcpy(g_img_errmsg,
                  pump.fail == RXF_INFLATE ? "dvzip inflate failed"
                  : pump.fail == RXF_OOM   ? "out of PSRAM"
                                           : "file too large",
                  sizeof(g_img_errmsg));
          g_img_err++;
          goto done;
        }
        if (pump.out_len) {
          uint8_t* shr2 = (uint8_t*)heap_caps_realloc(pump.out, pump.out_len, MALLOC_CAP_SPIRAM);
          if (shr2) pump.out = shr2;
        }
        outbuf = pump.out;
        pump.out = nullptr;
        cpio = outbuf;
        cpio_len = pump.out_len;
        AD_LOG("DVZIP: %lu records (%lu stored), %lu in -> %lu out, end=%s (post-hoc)\n", (unsigned long)pump.nrec,
               (unsigned long)pump.nstored, (unsigned long)len, (unsigned long)cpio_len, ad_dvzip_end_name(pump.end));
        /* A WALK THAT STOPPED IS NOT A WALK THAT FINISHED.
           This used to return the buffer and say nothing: 1,310,889 bytes of a ~2,354,000
           byte archive reached the caller labelled good, and the only reason anyone found
           out was that a cpio walker two layers away happened to notice. The whole files
           inside a short archive are still real files, so they are still delivered -- but
           the archive is not called complete. */
        if (!ad_dvzip_end_ok(pump.end)) {
          snprintf(g_img_errmsg, sizeof(g_img_errmsg), "dvzip %s at %lu of %lu", ad_dvzip_end_name(pump.end),
                   (unsigned long)cpio_len, (unsigned long)len);
          g_img_err++;
        }
      } else {
        strlcpy(g_img_errmsg, dvzip ? "out of PSRAM" : "not a cpio/gzip", sizeof(g_img_errmsg));
        g_img_err++;
        goto done;
      }
    }

    {  // dump the reconstructed cpio head so we can see the format/first entry
      char hx[3 * 48 + 1] = {0};
      int hn = (cpio_len < 48) ? (int)cpio_len : 48;
      for (int i = 0; i < hn; i++) sprintf(hx + i * 3, "%02x ", cpio[i]);
      AD_LOG("CPIO: %lu bytes head: %s\n", (unsigned long)cpio_len, hx);
    }
    if (g_arch_ready) {
      // A previous archive is still waiting to be handed over. Dropping the new one is
      // right -- the alternative is freeing a block the consumer may be inside.
      if (outbuf)
        heap_caps_free(outbuf);
      else if (gz) {
        heap_caps_free(gz);
        gz = nullptr;
      }
      g_img_err++;
      strlcpy(g_img_errmsg, "previous not collected", sizeof(g_img_errmsg));
    } else {
      g_arch_block = outbuf ? outbuf : gz;
      gz = nullptr;
      outbuf = nullptr;  // ownership transferred
      g_arch_buf = cpio;
      g_arch_len = cpio_len;
      g_upload_ok++;
      g_arch_ready = true;  // release flag: set LAST
      AD_LOG("UPLOAD-OK! %lu bytes of cpio -> handler\n", (unsigned long)cpio_len);
    }
  }

done:
  if (g_stream_cb && g_img_err != err0) stream_deliver(AdStreamEvent::Abort);
  if (!response_sent) {
    // The UI task owns SD access. Wait for its atomic archive-release acknowledgement.
    const uint32_t saving_started = millis();
    while (g_arch_ready.load() && !g_stop.load() && millis() - saving_started < 10000) vTaskDelay(pdMS_TO_TICKS(5));
    const bool saved = !g_arch_ready.load() && !g_stop.load() && g_img_err == err0;
    const char* status = saved ? "200 OK" : "507 Insufficient Storage";
    char reply[128];
    const int length =
        snprintf(reply, sizeof(reply), "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
    ssl_write_all(ssl, reinterpret_cast<const unsigned char*>(reply), length);
  }
  if (gz) heap_caps_free(gz);
  if (pump.out) heap_caps_free(pump.out);                           // a decode block that never reached handoff
  if (g_img_err != err0) AD_LOG("UPLOAD-ERR: %s\n", g_img_errmsg);  // was silent before
  g_upload_pending.store(false);
  g_rx_active = false;
  return;
}

// HTTP phase: runs AFTER the TLS handshake completes, on the committed connection
// (fd set blocking + recv_timeout bio by the caller). Serves /Discover -> tile,
// /Ask -> accept, /Upload -> receive. sharingd reuses ONE TLS connection for the
// /Ask -> /Upload pair, so we loop over requests until it closes. A single
// persistent buffered reader spans the whole connection: request bodies (incl.
// the chunked /Ask metadata) are drained through it, so the next request starts
// exactly where the last one ended -- no bytes are read-ahead and lost.
static bool peer_address(uint8_t out[16]) {
  sockaddr_in6 peer{};
  socklen_t len = sizeof(peer);
  if (getpeername(g_net.fd, reinterpret_cast<sockaddr*>(&peer), &len) != 0) return false;
  memcpy(out, &peer.sin6_addr, 16);
  return true;
}
static bool sv_cancelled(struct HttpServe*) { return g_stop.load(); }
static const char* sv_prepare_ask(struct HttpServe* sv, bool chunked, int clen) {
  g_approved = false;
  g_request_started_at = millis();
  // TLS owns this bounded PSRAM block only during metadata parsing, before upload scratch exists.
  constexpr uint32_t cap = 512u * 1024u;
  auto* body = static_cast<uint8_t*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
  if (!body) {
    g_img_err++;
    return "507 Insufficient Storage";
  }
  uint32_t length = 0;
  AdAskInfo info;
  AdAskParser parser;
  const bool valid = http_read_ask(sv, chunked, clen, body, cap, &length) && parser.parse(body, length, info);
  heap_caps_free(body);
  if (!valid) {
    strlcpy(g_img_errmsg, "invalid request metadata", sizeof(g_img_errmsg));
    g_img_err++;
    return "400 Bad Request";
  }
  if (info.totalBytes > g_rx_max) {
    g_img_err++;
    g_limit_rejections.fetch_add(1);
    return "413 Content Too Large";
  }
  portENTER_CRITICAL(&g_request_lock);
  g_request_info = info;
  portEXIT_CRITICAL(&g_request_lock);
  return nullptr;
}
static bool sv_approve(struct HttpServe*) {
  g_approved = false;
  if (g_arch_ready || g_stop.load() || !peer_address(g_approved_peer)) return false;
  g_consent.store(Consent::Asking);
  const uint32_t start = g_request_started_at;
  while (g_consent.load() == Consent::Asking && !g_stop.load() && millis() - start < 40000)
    vTaskDelay(pdMS_TO_TICKS(20));
  const Consent result = g_consent.exchange(Consent::Idle);
  g_approved = result == Consent::Accept && !g_stop.load();
  g_approved_at = millis();
  return g_approved;
}
static bool sv_authorized(struct HttpServe*) {
  uint8_t peer[16];
  const bool ok =
      g_approved && millis() - g_approved_at < 90000 && peer_address(peer) && memcmp(peer, g_approved_peer, 16) == 0;
  g_approved = false;
  return ok;
}
static void sv_upload(struct HttpServe* s, bool chunked, int clen) { rx_upload(s->r, chunked, clen); }
/* Zero-timeout look at the listening socket: is the upload connection already sitting in
   the backlog? It is how the /Ask drain knows the sender has read our 200 -- see the note
   at the drain in ad_serve.h. */
static int g_listen_fd = -1;
static bool sv_peer_waiting(struct HttpServe*) {
  if (g_listen_fd < 0) return false;
  fd_set rf;
  FD_ZERO(&rf);
  FD_SET(g_listen_fd, &rf);
  struct timeval tv = {0, 0};
  return select(g_listen_fd + 1, &rf, nullptr, nullptr, &tv) > 0 && FD_ISSET(g_listen_fd, &rf);
}

static void sv_on_ask_sent(struct HttpServe*) {
  g_rx_active = true;
  g_rx_bytes = 0;  // switch the badge to the "Receiving" screen
  g_rx_touch_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

static void serve_http(mbedtls_ssl_context* ssl) {
  struct HttpRdr r;
  http_rdr_init(&r, ssl, http_src_mbedtls);
  struct HttpServe sv;
  memset(&sv, 0, sizeof(sv));
  sv.r = &r;
  sv.io = ssl;
  sv.write = sv_write;
  sv.now = sv_now;
  sv.log = sv_log;
  sv.set_timeout = http_set_timeout;
  sv.disc_body = g_disc_body;
  sv.disc_len = (int)g_disc_len;
  sv.ask_body = g_ask_body;
  sv.ask_len = (int)g_ask_len;
  sv.disc_ms = &g_disc_ms;
  sv.ask_ms = &g_ask_ms;
  sv.disc_ok = &g_discover_ok;
  sv.ask_ok = &g_ask_ok;
  sv.desync = &g_desync;
  sv.on_ask_sent = sv_on_ask_sent;
  sv.upload = sv_upload;
  sv.peer_waiting = sv_peer_waiting;
  sv.prepare_ask = sv_prepare_ask;
  sv.cancelled = sv_cancelled;
  sv.approve = sv_approve;
  sv.authorized = sv_authorized;
  sv.enter_ms = g_http_enter_ms;
  serve_http_run(&sv);
}

// TLS acceptor as a single-task, single-session, non-blocking select() loop that
// "follows the ClientHello". sharingd opens ~4 PARALLEL connections (racing) and
// sends its real ClientHello on the one it commits to; a blocking one-at-a-time
// server was always stuck waiting on a dead connection while the live CH rotted
// unread. Here every fd is O_NONBLOCK; we select() over all of them, adopt the
// NEWEST fd that has a queued ClientHello (MSG_PEEK), step the handshake without
// blocking, and preempt a silent active connection if a newer one gets a CH.
// ClientHello witness: wrap the handshake-phase recv BIO and hexdump the first
// CH_DUMP_MAX bytes of each adopted connection's inbound stream ("CH <ms> off=N <hex>").
// This is the discriminating measurement for the cycle-start resumption misses: the
// CH is plaintext, so its session_id + extensions (supported_versions 0x2b, key_share
// 0x33, session_ticket 0x23, ALPN 0x10) settle at once (a) whether the unknown IDs
// are TLS1.3-compat random fill, (b) whether enabling server tickets could even
// negotiate, (c) whether ALPN/False Start is on the table, (d) whether the client
// offers TLS1.3 at all. Dumping the byte STREAM (not per-segment) reassembles a CH
// split across segments for free. The inbound segment ceiling is ~1220 B [D] -- the
// 1280-byte AWDL path MTU less 40 bytes of IPv6 and 20 of TCP header; nothing on this
// path records an actual inbound segment length, so that figure is arithmetic, not a
// measurement. 1600B may truncate an oversized CH -- the offline parser sees the
// record length and flags it; bump the cap and re-measure if that happens.
// Runs in tcp_listen_task (not process_frame),
// so the "no Serial in process_frame" invariant is untouched; worst case is one
// ~4.8KB burst per adoption, which only fires on real sharingd connections.
#ifdef AIRDROP_CH_DUMP
#define CH_DUMP_MAX 1600
static int g_chdump_left = 0;  // bytes still to dump for the current adoption
static int g_chdump_off = 0;   // cumulative stream offset, for offline reassembly
static int chdump_recv(void* ctx, unsigned char* buf, size_t len) {
  int r = mbedtls_net_recv(ctx, buf, len);
  if (r > 0 && g_chdump_left > 0) {
    int n = (r < g_chdump_left) ? r : g_chdump_left;
    for (int off = 0; off < n; off += 32) {
      char line[160];
      int m = (n - off < 32) ? (n - off) : 32;
      int p = snprintf(line, sizeof(line), "CH %lu off=%d ", (unsigned long)millis(), g_chdump_off + off);
      for (int i = 0; i < m && p < (int)sizeof(line) - 3; i++)
        p += snprintf(line + p, sizeof(line) - p, "%02x", buf[off + i]);
      line[p++] = '\n';
      ad_diag_stage(line, p);
    }
    g_chdump_off += n;
    g_chdump_left -= n;
    if (g_chdump_left == 0) EV("chdump end off=%d more=%d", g_chdump_off, r - n);
  }
  return r;
}
#endif /* AIRDROP_CH_DUMP */

#define MAXC 12
/* THE LISTENER'S ACTUAL STACK NEED. SETTLED: 32,768 allocated, 17,348 used at the peak.
 *
 * The task is created with 32,768 bytes. It was raised there from 4,096 because the mbedTLS
 * handshake overflowed the smaller stack and crashed, which establishes 4,096 is too little. What is enough is the
 * measurement above: a real transfer peaks at 17,348 bytes used, so 32,768 carries roughly 15 KB of headroom. The
 * number is worth having because the reserve is expensive -- 32,768 CONTIGUOUS bytes of
 * internal RAM is the largest single allocation the library makes, out of the same heap
 * mbedTLS wants two ~16.6 KB blocks from, so a sketch that fails to start the listener is
 * short of contiguous internal heap rather than of stack.
 *
 * Because the answer is known, the SAMPLING is behind AD_DIAG_DETAIL. It was the first
 * statement of the select loop's for(;;), so it ran ~10 times a second for the life of the
 * device to re-derive a constant. With the define off nothing writes g_ls_stack_min and
 * ad_listen_stack_min_free() reports 0, which is exactly what it reports before the task
 * has run; re-open the question by rebuilding with -DAD_DIAG_DETAIL.
 *
 * It is the minimum over the task's life, not the instant value: a handshake's peak is what
 * matters and it is over in a second. uxTaskGetStackHighWaterMark returns BYTES on
 * ESP-IDF, not words, whatever the AWDL layer's function name suggests. */
static volatile uint32_t g_ls_stack_min = 0xffffffffu;

uint32_t ad_listen_stack_min_free(void) { return (g_ls_stack_min == 0xffffffffu) ? 0 : g_ls_stack_min; }

static void tcp_listen_task(void*) {
  airdrop_tls_init();
  int ls = -1;
  int conn[MAXC];
  uint32_t born[MAXC];
  for (int i = 0; i < MAXC; i++) conn[i] = -1;
  int active = -1;
  uint32_t t_data = 0, t_hs0 = 0;
  while (!g_stop.load()) {
#ifdef AD_DIAG_DETAIL
    {
      uint32_t hw = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
      if (hw < g_ls_stack_min) g_ls_stack_min = hw;
    }
#endif
    if (!g_tls_ready) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      airdrop_tls_init();
      continue;
    }
    if (ls < 0) {
      ls = socket(AF_INET6, SOCK_STREAM, 0);
      if (ls < 0) {
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }
      int one = 1;
      setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
      struct sockaddr_in6 a;
      memset(&a, 0, sizeof(a));
      a.sin6_family = AF_INET6;
      a.sin6_port = htons(AIRDROP_PORT);  // in6addr_any
      if (bind(ls, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(ls, 8) != 0) {
        close(ls);
        ls = -1;
        g_listen_fd = -1;
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      set_nonblock(ls, true);
      g_listen_fd = ls;  // published for sv_peer_waiting(), which ends the /Ask drain
      AD_LOG("[tcp] TLS listening (select loop) on [::]:%d\n", AIRDROP_PORT);
    }

    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(ls, &rf);
    int mx = ls;
    for (int i = 0; i < MAXC; i++)
      if (conn[i] >= 0) {
        FD_SET(conn[i], &rf);
        if (conn[i] > mx) mx = conn[i];
      }
    struct timeval tv = {0, 100000};  // 100ms tick
    select(mx + 1, &rf, NULL, NULL, &tv);
    uint32_t now = millis();

    // 1. Drain the accept backlog (non-blocking); TCP_NODELAY + O_NONBLOCK each.
    // The count here is diagnostic gold: while the task is inside serve_http it does
    // not call accept() at all, so connections sharingd opened during that window
    // sit in lwIP's listen backlog and all appear at once on the next pass. A
    // non-zero `since_http` on this line means we were deaf when the sender knocked.
    int n_acc_this_pass = 0;
    for (;;) {
      struct sockaddr_in6 c;
      socklen_t cl = sizeof(c);
      int fd = accept(ls, (struct sockaddr*)&c, &cl);
      if (fd < 0) break;
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      set_nonblock(fd, true);
      g_tcp_accept++;
      n_acc_this_pass++;
      EV("accept fd=%d port=%u since_http=%lu", fd, ntohs(c.sin6_port),
         (unsigned long)(g_http_exit_ms ? (now - g_http_exit_ms) : 0));
      int slot = -1;
      for (int i = 0; i < MAXC; i++)
        if (conn[i] < 0) {
          slot = i;
          break;
        }
      if (slot < 0) {  // full: evict oldest non-active
        int old = -1;
        uint32_t oldest = 0xffffffff;
        for (int i = 0; i < MAXC; i++)
          if (i != active && born[i] < oldest) {
            oldest = born[i];
            old = i;
          }
        if (old >= 0) {
          close(conn[old]);
          conn[old] = -1;
          slot = old;
        }
      }
      if (slot >= 0) {
        conn[slot] = fd;
        born[slot] = now;
      } else {
        EV("drop fd=%d table_full", fd);
        close(fd);
      }
    }
    if (n_acc_this_pass > 1)
      EV("backlog n=%d since_http=%lu -- sender knocked while we were deaf", n_acc_this_pass,
         (unsigned long)(g_http_exit_ms ? (now - g_http_exit_ms) : 0));

    // 2. Among fds select() marked READABLE, reap the EOF/error ones and pick the
    //    NEWEST that has a ClientHello queued. (Only peek readable fds -- an idle
    //    fd with no data yet must NOT be touched/closed; relying on errno==EAGAIN
    //    to tell "no data" from "closed" was closing idle connections before their
    //    ClientHello arrived, so the loop never adopted anything.)
    int cand = -1;
    uint32_t cand_born = 0;
    for (int i = 0; i < MAXC; i++) {
      if (conn[i] < 0 || i == active) continue;
      if (!FD_ISSET(conn[i], &rf)) continue;  // not readable -> still waiting for CH
      unsigned char b;
      int r = recv(conn[i], &b, 1, MSG_PEEK);
      if (r <= 0) {
        close(conn[i]);
        conn[i] = -1;
        continue;
      }  // EOF/error -> reap
      if (cand < 0 || born[i] > cand_born) {
        cand = i;
        cand_born = born[i];
      }
    }

    // 3. Elect (no active) or preempt (active silent >1.2s AND a newer CH waits).
    bool adopt = false;
    if (active < 0 && cand >= 0)
      adopt = true;
    else if (active >= 0 && cand >= 0 && cand != active && born[cand] > born[active] &&
             (uint32_t)(now - t_data) > 1200) {
      AD_LOG("[serve] preempt fd=%d -> fd=%d\n", conn[active], conn[cand]);
      close(conn[active]);
      conn[active] = -1;
      active = -1;
      adopt = true;
    }
    /* The select loop's own heartbeat: how many fds are held, how many are readable, and
       which one the election above picked. It is gated only by a 1000 ms timer, so it fires
       whether or not any connection exists, and it spends one of the ring's eight slots
       every second -- against lines like `adopt`, `cull` and `hs_fail`, which arrive in
       bursts and are the reason anyone reads the ring at all. Nothing in the tree parses
       "[loop]"; it is for a human watching an election go wrong, so it is behind
       AD_DIAG_DETAIL and off unless someone is watching. */
#ifdef AD_DIAG_DETAIL
    {
      static uint32_t t_dbg = 0;
      if ((uint32_t)(now - t_dbg) >= 1000) {
        t_dbg = now;
        int nc = 0, rd = 0;
        for (int i = 0; i < MAXC; i++)
          if (conn[i] >= 0) {
            nc++;
            if (FD_ISSET(conn[i], &rf)) rd++;
          }
        AD_LOG("[loop] nconn=%d rdbl=%d cand=%d active=%d acc=%lu\n", nc, rd, cand, active,
               (unsigned long)g_tcp_accept);
      }
    }
#endif
    if (adopt) {
      mbedtls_ssl_session_reset(&g_ssl);
      g_net.fd = conn[cand];
#ifdef HTTP_TRACE
      ht_arm();  // arm the decrypted-stream witness
#endif
#ifdef AIRDROP_CH_DUMP
      g_chdump_left = CH_DUMP_MAX;
      g_chdump_off = 0;  // arm the CH witness
      mbedtls_ssl_set_bio(&g_ssl, &g_net, mbedtls_net_send, chdump_recv, NULL);
#else
      mbedtls_ssl_set_bio(&g_ssl, &g_net, mbedtls_net_send, mbedtls_net_recv, NULL);
#endif
      active = cand;
      t_hs0 = t_data = now;
      EV("adopt fd=%d age=%lu heap=%u", conn[cand], (unsigned long)(now - born[cand]),
         (unsigned)esp_get_free_internal_heap_size());
    }

    // 4. Step the handshake on the active connection (non-blocking).
    if (active >= 0 && conn[active] >= 0) {
      if (FD_ISSET(conn[active], &rf)) t_data = now;
      int r = mbedtls_ssl_handshake(&g_ssl);
      if (r == 0) {
        g_tls_ok++;
        EV("tlsok fd=%d %s hs_ms=%lu heap=%u", conn[active], mbedtls_ssl_get_version(&g_ssl),
           (unsigned long)(now - t_hs0), (unsigned)esp_get_free_internal_heap_size());
        int afd = conn[active];
        int killed = 0;
        for (int i = 0; i < MAXC; i++)
          if (conn[i] >= 0 && i != active) {
            close(conn[i]);
            conn[i] = -1;
            killed++;
          }
        if (killed) EV("cull n=%d -- closed sharingd's other parallel connections", killed);
        set_nonblock(afd, false);  // HTTP phase: blocking + read timeout
        mbedtls_ssl_set_bio(&g_ssl, &g_net, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);
        g_http_enter_ms = millis();
        EV("http_enter fd=%d", afd);
        serve_http(&g_ssl);
        g_http_exit_ms = millis();
        EV("http_exit fd=%d blocked_ms=%lu since_disc=%lu", afd, (unsigned long)(g_http_exit_ms - g_http_enter_ms),
           (unsigned long)(g_disc_ms ? (g_http_exit_ms - g_disc_ms) : 0));
        mbedtls_ssl_close_notify(&g_ssl);
        close(afd);
        conn[active] = -1;
        active = -1;
        mbedtls_ssl_session_reset(&g_ssl);
      } else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
        EV("hs_fail fd=%d err=-0x%04x", conn[active], -r);
        close(conn[active]);
        conn[active] = -1;
        active = -1;
        mbedtls_ssl_session_reset(&g_ssl);
      }
    } else
      active = -1;

    // 5. Handshake deadline: drop a connection that never finishes.
    if (active >= 0 && (uint32_t)(now - t_hs0) > 15000) {
      AD_LOG("[tls] handshake deadline (fd=%d)\n", conn[active]);
      close(conn[active]);
      conn[active] = -1;
      active = -1;
      mbedtls_ssl_session_reset(&g_ssl);
    }
  }
  for (int i = 0; i < MAXC; i++)
    if (conn[i] >= 0) close(conn[i]);
  if (ls >= 0) close(ls);
  g_listen_fd = -1;
  g_net.fd = -1;
  airdrop_tls_release();
  g_listener_running.store(false);
  vTaskDeleteWithCaps(nullptr);
}

/* ==========================================================================
 * The seam. Everything above this line moved unchanged; everything below it is
 * new, and it is new because twenty file-scope globals used to cross here.
 * ========================================================================== */

/* --- the received file ---------------------------------------------------- */

static ad_file_cb_t g_file_cb = nullptr;
static void* g_file_ctx = nullptr;

void ad_on_file(ad_file_cb_t cb, void* ctx) {
  g_file_cb = cb;
  g_file_ctx = ctx;
}

/* The consumer half. The TLS task published a whole cpio archive and did no parsing; this
 * walks it and hands over every regular file, on the CALLER's task.
 *
 * The producer ordering in rx_upload -- block, then buf/len, then g_arch_ready LAST -- is
 * the release flag this relies on, and it is unchanged.
 *
 * ONE CALLBACK PER FILE, and the type is never a filter. What this replaces returned the
 * first JPEG or PNG in the archive and threw the rest away: a PDF, a text file, a contact
 * card, or the second of two photos disappeared, and the transfer was reported as "no
 * displayable image" rather than as delivered. The archives in the capture corpus are all
 * two entries -- a zero-byte "." directory and the file -- which is why the directory is
 * skipped by MODE rather than by guessing at names or at zero length.
 *
 * The PSRAM block is freed once, after the last file, which is why AdFile::data is
 * documented as callback-lifetime only. */
void ad_poll(void) {
  auto pending = StreamSlot::Pending;
  if (g_stream_slot.compare_exchange_strong(pending, StreamSlot::Processing)) {
    g_stream_result = g_stream_cb && g_stream_cb(g_stream_ctx, g_stream_event, g_stream_data, g_stream_len);
    g_stream_slot.store(StreamSlot::Idle);
  }
  if (!g_arch_ready) return;

  const uint8_t* buf = g_arch_buf;
  uint32_t blen = g_arch_len;

  /* Count first, so a handler can say "file 2 of 3" without the library having to promise
     an ordering it does not control. Two passes over an in-RAM buffer costs microseconds. */
  uint32_t total = 0;
  {
    struct AdCpioIter it;
    struct AdCpioEntry e;
    ad_cpio_begin(&it, buf, blen);
    while (ad_cpio_next(&it, &e))
      if (ad_cpio_is_regular(e.mode)) total++;
  }

  uint32_t idx = 0;
  bool any_refused = false;
  {
    struct AdCpioIter it;
    struct AdCpioEntry e;
    ad_cpio_begin(&it, buf, blen);
    while (ad_cpio_next(&it, &e)) {
      if (!ad_cpio_is_regular(e.mode)) continue; /* the "." every transfer carries */
      /* The archive's name is not NUL-terminated in a way we should trust, so it is copied
         into a bounded buffer rather than handed over as a pointer into remote bytes. */
      char path[128], name[256];
      uint32_t pl = e.name_len < sizeof(path) - 1 ? e.name_len : sizeof(path) - 1;
      memcpy(path, e.name, pl);
      path[pl] = 0;
      uint32_t bl;
      const char* b = ad_cpio_basename(e.name, e.name_len, &bl);
      if (bl >= sizeof(name)) {
        g_img_err++;
        any_refused = true;
        continue;
      }
      memcpy(name, b, bl);
      name[bl] = 0;

      struct AdFile f;
      f.data = e.data;
      f.len = e.len;
      f.name = name;
      f.path = path;
      f.type = ad_cpio_sniff(e.data, e.len);
      f.index = idx++;
      f.count = total;

      bool ok = g_file_cb ? g_file_cb(g_file_ctx, &f) : false;
      if (ok)
        g_files_ok++;
      else {
        g_img_err++;
        any_refused = true;
      }
    }
    if (it.truncated) {
      g_img_err++;
      strlcpy(g_img_errmsg, "archive truncated", sizeof(g_img_errmsg));
      AD_LOG("CPIO-CUT: stopped at %lu of %lu after %lu regular files\n", (unsigned long)it.pos, (unsigned long)blen,
             (unsigned long)total);
    } else if (total == 0) {
      g_img_err++;
      strlcpy(g_img_errmsg, "archive held no files", sizeof(g_img_errmsg));
    } else if (any_refused) {
      strlcpy(g_img_errmsg, "handler refused a file", sizeof(g_img_errmsg));
    }
  }

  if (g_arch_block) {
    heap_caps_free(g_arch_block);
    g_arch_block = nullptr;
  }
  g_arch_buf = nullptr;
  g_arch_len = 0;
  g_arch_ready = false;
  g_rx_active = false;
}

/* --- lifecycle ------------------------------------------------------------ */

static bool g_ad_role_receiver = false;
bool ad_role_is_receiver(void) { return g_ad_role_receiver; }

int ad_begin(const char* name, uint32_t max_receive) {
  /* The roles are exclusive: a receiver holds a 32 KB listener stack and a sender holds a
     TLS client context for its whole life, and neither leaves room for the other beside
     AWDL. Recording which one claimed the device lets the second attempt be refused
     instead of half-working in a way nothing reports. */
  if (g_listener_running.load()) return -1;
  g_stop.store(false);
  g_consent.store(Consent::Idle);
  g_approved = false;
  g_ad_role_receiver = true;

  /* 0 means the library's own default, RX_MAX_AUTO (6 MiB); PSRAM can only lower what is
     actually granted, and AdStatus.rx_max reports it. A tiny value is honoured as given -- refusing to accept
     anything is a legitimate thing for a sketch to want, and second-guessing it would just
     hide the setting. What PSRAM cannot supply is reported through AdStatus.rx_max, not
     silently substituted. */
  g_rx_max = max_receive ? max_receive : RX_MAX_AUTO;
  awdl_ring_init(&g_adlog.cur, AD_DIAG_SLOTS); /* before the task that fills it */

  /* The name is what a sender sees. Refusing a bad one here, before anything is
     advertised, beats advertising something the user did not ask for: a name that cannot
     be encoded is a mistake worth reporting, not worth papering over. */
  if (!name || !*name) name = "ESP32";
  strlcpy(g_name, name, sizeof(g_name));
  if (!ad_build_identity(g_name)) return -2;

  /* AWDL must already be up: the identity below is derived from the interface MAC, and
     the mDNS records we are about to advertise are built out of it. */
  awdl_identity(&g_id);
  awdl_rx_tap_set(ad_rx_tap, nullptr);
  /* No announce callback is registered -- there is no periodic multicast announcement, for
     the reasons set out in the note above ad_respond(). The hook itself stays available. */
  // The listener performs no flash writes; its 32 KiB stack can use PSRAM.
  // mbedTLS heap allocations still use the SDK internal-RAM allocator.
  g_listener_running.store(true);
  if (xTaskCreatePinnedToCoreWithCaps(tcp_listen_task, "tcp_listen", 32768, nullptr, 2, nullptr, 1,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    g_listener_running.store(false);
    g_ad_role_receiver = false;
    return -1;
  }
  return 0;
}

bool ad_end() {
  g_stop.store(true);
  ad_request_answer(false);
  if (g_listener_running.load() && g_net.fd >= 0) shutdown(g_net.fd, SHUT_RDWR);
  const uint32_t start = millis();
  while (g_listener_running.load() && millis() - start < 7000) vTaskDelay(pdMS_TO_TICKS(10));
  if (g_listener_running.load()) return false;
  if (g_tls_tried) airdrop_tls_release();
  awdl_rx_tap_set(nullptr, nullptr);
  if (g_arch_block) heap_caps_free(g_arch_block);
  g_arch_block = nullptr;
  g_arch_buf = nullptr;
  g_arch_len = 0;
  g_arch_ready = false;
  g_rx_active = false;
  g_ad_role_receiver = false;
  g_approved = false;
  ad_on_file(nullptr, nullptr);
  ad_on_stream(nullptr, nullptr);
  g_stream_slot.store(StreamSlot::Idle);
  return true;
}

/* --- telemetry ------------------------------------------------------------ */

void ad_stats_read(struct AdStatus* out) {
  out->tcp_accept = g_tcp_accept;
  out->tls_ok = g_tls_ok;
  out->discover_ok = g_discover_ok;
  out->ask_ok = g_ask_ok;
  out->upload_ok = g_upload_ok;
  out->desync = g_desync;
  /* Recency, for the same reason AwdlStatus needed it: a state with no clock behind it
     cannot notice that it stopped being true. */
  {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    out->rx_active = g_rx_active && (uint32_t)(now_ms - g_rx_touch_ms) <= AD_RX_STALL_MS;
  }
  out->rx_bytes = g_rx_bytes;
  out->rx_max = g_rx_max_eff;
  out->files_ok = g_files_ok;
  out->limit_rejections = g_limit_rejections.load();
  out->file_err = g_img_err;
  strlcpy(out->file_errmsg, g_img_errmsg, sizeof(out->file_errmsg));
  out->tls_alg = g_tls_alg;
  out->ec_crt_err = g_ec_crt_err;
  out->ec_key_err = g_ec_key_err;
  out->query_seen = g_query_seen;
  out->resp_tx = g_resp_tx;
  out->notus = g_notus;
  out->ka_suppress = g_ka_suppress;
  out->dump_count = g_dump_count;
  out->tcp_tous = g_tcp_tous;
  out->syn_airdrop = g_syn_airdrop;
  out->syn_pair1 = g_syn_pair1;
  out->syn_pair2 = g_syn_pair2;
  out->syn_other = g_syn_other;
  out->syn_other_port = g_syn_other_port;
}

#else /* ESP32DROP_SENDER */

/* A sender build. The receiving implementation is not here -- see the note at the top of
   this file. Nothing is stubbed out: ad_begin(), ad_poll() and the rest simply do not
   exist, so calling one is a link error rather than a silent waste of 32 KB.
   ad_role_is_receiver() is the single exception, because ad_sender_begin() asks it. */
#include "ad_port_esp32.h"
bool ad_role_is_receiver(void) { return false; }

#endif /* !ESP32DROP_SENDER */
