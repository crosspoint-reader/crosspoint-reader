#include <freertos/idf_additions.h>

#include <algorithm>

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
#include <wolfssl/ssl.h>

#include <atomic>

#include "esp_heap_caps.h" /* heap_caps_malloc(..., MALLOC_CAP_SPIRAM): decode in PSRAM */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

/* The ONE include from src/awdl, and the rule permits exactly this one: the public
   header. It brings awdl_ring.h, whose SPSC cursor the diagnostic ring below reuses
   rather than growing a second, untested copy. */
#include "../../ESP32AWDL.h"
#include "../core/ad_dvzip.h"
#include "../core/ad_http.h"
#include "../core/ad_identity.h"
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
static volatile uint32_t g_notus = 0, g_ka_suppress = 0;
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
 * Only nonblocking frame inspection and awdl_tx_ip6_now() are allowed.
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
          g_notus++;
        } else if (ad_dnssd_known_answer_suppresses(dns, dns_len, &own)) {
          g_ka_suppress++;
        } else {
          ad_respond(src);
        }
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

static uint32_t g_disc_ms = 0;  // when we last answered /Discover
static uint32_t g_ask_ms = 0;   // when we last answered /Ask (the /Upload clock)

// Transfer progress is updated by the listener and read by the main task.
#define AD_RX_STALL_MS 20000u
static volatile uint32_t g_rx_touch_ms = 0;
static volatile bool g_rx_active = false;  // transfer in progress: "Receiving" UI
static volatile uint32_t g_rx_bytes = 0;   // compressed bytes received so far
static volatile uint32_t g_files_ok = 0;
static volatile uint32_t g_img_err = 0;  // undecodable/oversize transfers
static char g_img_errmsg[40] = {0};
// The caller supplies the archive limit; zero selects the library default.
#define RX_MAX_AUTO (6u * 1024 * 1024)
static uint32_t g_rx_max = RX_MAX_AUTO;    /* policy, set by ad_begin() */
static volatile uint32_t g_rx_max_eff = 0; /* what a transfer actually got */

// ---- TLS server config, built once ------------------------------------------
static const char* g_tls_alg = "none";
static int g_ec_crt_err = 0, g_ec_key_err = 0;  // wolfSSL certificate/key load results
static WOLFSSL_CTX* g_tlsconf = nullptr;
static WOLFSSL* g_ssl = nullptr;
static int g_tls_fd = -1;
static bool g_tls_ready = false;
static std::atomic<bool> g_stop{false}, g_listener_running{false};
enum class Consent { Idle, Asking, Accept, Decline };
static std::atomic<Consent> g_consent{Consent::Idle};
static uint8_t g_approved_peer[16];
static uint32_t g_approved_at = 0;
static std::atomic<bool> g_approved{false};
static std::atomic<uint32_t> g_socket_rx_bytes{0}, g_tls_failures{0};
static std::atomic<int> g_last_tls_error{0};
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

// wolfSSL is already used by CrossPoint's HTTPS transport. This session owns
// only its context and connection; it never changes the process-wide allocator.
static int tls_recv(WOLFSSL*, char* data, int len, void*) {
  const int n = recv(g_tls_fd, data, len, 0);
  if (n > 0) {
    g_socket_rx_bytes.fetch_add(n);
    return n;
  }
  if (!n) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return WOLFSSL_CBIO_ERR_WANT_READ;
  return WOLFSSL_CBIO_ERR_GENERAL;
}
static int tls_send(WOLFSSL*, char* data, int len, void*) {
  const int n = send(g_tls_fd, data, len, 0);
  if (n > 0) return n;
  if (!n) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return WOLFSSL_CBIO_ERR_WANT_WRITE;
  return WOLFSSL_CBIO_ERR_GENERAL;
}
static bool tls_want_io(int error) { return error == WOLFSSL_ERROR_WANT_READ || error == WOLFSSL_ERROR_WANT_WRITE; }
static void tls_reset_session() {
  if (g_ssl) wolfSSL_free(g_ssl);
  g_ssl = nullptr;
}
static void airdrop_tls_release() {
  tls_reset_session();
  if (g_tlsconf) wolfSSL_CTX_free(g_tlsconf);
  g_tlsconf = nullptr;
  g_tls_ready = false;
}
static void airdrop_tls_init() {
  if (g_tls_ready) return;
  airdrop_tls_release();
  g_tlsconf = wolfSSL_CTX_new(wolfTLSv1_2_server_method());
  if (!g_tlsconf) return;
  // Everyone-mode AirDrop uses a self-signed server certificate, without client authentication.
  wolfSSL_CTX_set_verify(g_tlsconf, WOLFSSL_VERIFY_NONE, nullptr);
  g_ec_crt_err =
      wolfSSL_CTX_use_certificate_buffer(g_tlsconf, AIRDROP_CRT_EC_DER, AIRDROP_CRT_EC_DER_LEN, WOLFSSL_FILETYPE_ASN1);
  g_ec_key_err =
      wolfSSL_CTX_use_PrivateKey_buffer(g_tlsconf, AIRDROP_KEY_EC_DER, AIRDROP_KEY_EC_DER_LEN, WOLFSSL_FILETYPE_ASN1);
  if (g_ec_crt_err != WOLFSSL_SUCCESS || g_ec_key_err != WOLFSSL_SUCCESS) {
    airdrop_tls_release();
    return;
  }
  wolfSSL_SetIORecv(g_tlsconf, tls_recv);
  wolfSSL_SetIOSend(g_tlsconf, tls_send);
  wolfSSL_CTX_set_session_cache_mode(g_tlsconf, WOLFSSL_SESS_CACHE_SERVER);
  g_ssl = wolfSSL_new(g_tlsconf);
  if (!g_ssl) {
    airdrop_tls_release();
    return;
  }
  g_tls_alg = "ECDSA";
  g_tls_ready = true;
}
bool ad_prepare_tls() {
  if (g_listener_running.load()) return false;
  airdrop_tls_init();
  return g_tls_ready;
}
static int ssl_write_all(WOLFSSL* ssl, const unsigned char* p, int len) {
  int off = 0;
  const uint32_t start = millis();
  while (off < len && !g_stop.load() && millis() - start < 10000) {
    const int n = wolfSSL_write(ssl, p + off, len - off);
    if (n > 0) {
      off += n;
      continue;
    }
    if (!tls_want_io(wolfSSL_get_error(ssl, n))) return -1;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return off == len ? off : -1;
}
static void set_rd_timeout(uint32_t ms) {
  const timeval timeout = {static_cast<time_t>(ms / 1000), static_cast<suseconds_t>((ms % 1000) * 1000)};
  setsockopt(g_tls_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(g_tls_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}
// HTTP uses blocking sockets: WANT_READ after a socket timeout means no bytes,
// while a closed connection or a TLS error must poison the request.
static int http_src_tls(struct HttpRdr* r, unsigned char* buf, int len) {
  auto* ssl = static_cast<WOLFSSL*>(r->ctx);
  const int n = wolfSSL_read(ssl, buf, len);
  if (n > 0) return n;
  const int error = wolfSSL_get_error(ssl, n);
  if (tls_want_io(error)) return 0;
  g_last_tls_error.store(error);
  return -1;
}
static void http_set_timeout(struct HttpRdr* r, uint32_t ms) {
  r->timeout_ms = ms;
  set_rd_timeout(ms);
}

// Firmware adapters for the transport-agnostic dispatch layer (ad_serve.h).
// serve() below fills an HttpServe with these; the same struct on the host test
// side points write/now/log at a capture buffer and a scripted clock.
static int sv_write(struct HttpServe* s, const unsigned char* p, int n) { return ssl_write_all((WOLFSSL*)s->io, p, n); }
static uint32_t sv_now(struct HttpServe*) { return millis(); }

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
// Chunked and Content-Length bodies use the same DvZip record pump. Fixed
// scratch is handed to ad_poll through the stream callback for incremental storage.
// The TLS task never touches storage or the display. Receipt failures precede
// the response; success waits for the consumer's final validation.

enum {
  RXM_UNDECIDED = 0,  // fewer than 10 body bytes: no shape yet
  RXM_STREAM,         // DvZip signature seen: decode records as they complete
  RXM_RAW
};  // unsupported body signature

enum {
  RXF_NONE = 0,
  RXF_OOM,  // this device could not give the memory -> 507
  RXF_CAP,  // the body outgrew what this device can ever hold -> 413
  RXF_INFLATE
};  // a record's zlib would not inflate -> 400

struct RxPump {
  uint8_t* out;  // fixed record scratch, owned by RxPump until release
  uint32_t out_len, out_cap;
  uint32_t in_total;      // body bytes ever appended -- sink->len is only the residue
  uint32_t residue_prev;  // sink->len as this hook last left it, for the delta
  uint32_t nrec, nstored;
  int end;       // AdDvzipEnd of the FINAL drain; init to END_RUNNING
  uint8_t mode;  // RXM_*
  uint8_t fail;  // RXF_*, sticky
};

// Decode one bounded record and deliver it through the main-task mailbox.
static bool rx_pump_emit(void* vp, const struct AdDvzipRec* rec, const uint8_t* payload) {
  struct RxPump* p = (struct RxPump*)vp;
  uint32_t room = 256u * 1024u;
  uint8_t* target = p->out;
  if (rec->stored) {
    /* A STORED RECORD IS A COPY, NOT A DECOMPRESSION. macOS switches to these once
       deflate stops paying on an already-compressed image -- measured, 6 of the 18
       records in a 2.35 MB photo. Feeding one to the inflater is what used to cut the
       stream and deliver half an archive. */
    if (rec->len > room) {
      p->fail = RXF_INFLATE;

      return false;
    }
    memcpy(target, payload, rec->len);
    if (rec->len > p->out_cap - p->out_len || !stream_deliver(AdStreamEvent::Data, target, rec->len)) {
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

    return false;
  }
  if (got > p->out_cap - p->out_len || !stream_deliver(AdStreamEvent::Data, target, got)) {
    p->fail = got > p->out_cap - p->out_len ? RXF_CAP : (g_stream_oversized.load() ? RXF_CAP : RXF_OOM);
    return false;
  }
  p->out_len += (uint32_t)got;
  p->nrec++;
  return true;
}

// Accept bounded DvZip records; unsupported body formats fail archive validation.
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

static bool rx_pump_open(struct RxPump* p) {
  if (g_stream_cb) {
    p->out_cap = g_rx_max;
    g_rx_max_eff = g_rx_max;
    // Port owns one fixed PSRAM scratch block; rx_upload frees it on every exit.
    p->out = (uint8_t*)heap_caps_malloc(256u * 1024u, MALLOC_CAP_SPIRAM);
    if (!p->out || !stream_deliver(AdStreamEvent::Begin)) {
      p->fail = RXF_OOM;
      return false;
    }

    return true;
  }
  p->fail = RXF_OOM;
  return false;
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
    if (p->fail == RXF_OOM) g_rx_grow_failed = true; /* -> 507, not 413 */
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
  WOLFSSL* ssl = (WOLFSSL*)r->ctx;
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

        break;
      case HTTP_RECV_OK_LAST_CHUNK:
        clean_end = true;

        break;
      case HTTP_RECV_OK_SILENCE:
        clean_end = true;

        break;
      case HTTP_RECV_OVERFLOW:
        overflow = true;
        break;
      default:
        /* The reason was thrown away here, which is why two identical 2,352,506-byte
           failures could not be told apart. rc IS the reason. */

        break;
    }

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
    if (pump.fail == RXF_OOM) g_rx_grow_failed = true;
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
    }
  }
  if (pump.fail == RXF_INFLATE) { /* before overflow, same order as the status */
    snprintf(g_img_errmsg, sizeof(g_img_errmsg), "record %lu would not inflate", (unsigned long)(pump.nrec + 1));
    g_img_err++;
    goto done;
  }
  if (overflow) {
    strlcpy(g_img_errmsg, g_rx_grow_failed ? "out of PSRAM" : "file too large", sizeof(g_img_errmsg));
    g_img_err++;
    if (pump.fail == RXF_CAP) g_limit_rejections.fetch_add(1);
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

  if (pump.out && ad_dvzip_end_ok(pump.end) && stream_deliver(AdStreamEvent::Finish)) {
    g_upload_ok++;
  } else {
    g_img_err++;
    strlcpy(g_img_errmsg, "stream incomplete or storage failed", sizeof(g_img_errmsg));
  }

done:
  if (g_stream_cb && g_img_err != err0) stream_deliver(AdStreamEvent::Abort);
  if (!response_sent) {
    const bool saved = !g_stop.load() && g_img_err == err0;
    const char* status = saved ? "200 OK" : "507 Insufficient Storage";
    char reply[128];
    const int length =
        snprintf(reply, sizeof(reply), "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
    ssl_write_all(ssl, reinterpret_cast<const unsigned char*>(reply), length);
  }
  if (gz) heap_caps_free(gz);
  if (pump.out) heap_caps_free(pump.out);  // a decode block that never reached handoff
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
  if (getpeername(g_tls_fd, reinterpret_cast<sockaddr*>(&peer), &len) != 0) return false;
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
  if (g_stop.load() || !peer_address(g_approved_peer)) return false;
  g_consent.store(Consent::Asking);
  const uint32_t start = g_request_started_at;
  while (g_consent.load() == Consent::Asking && !g_stop.load() && millis() - start < 40000)
    vTaskDelay(pdMS_TO_TICKS(20));
  const Consent result = g_consent.exchange(Consent::Idle);
  if (result == Consent::Asking && !g_stop.load()) {
    strlcpy(g_img_errmsg, "acceptance timed out", sizeof(g_img_errmsg));
    g_img_err++;
  }
  g_approved = result == Consent::Accept && !g_stop.load();
  g_approved_at = millis();
  return g_approved;
}
static void sv_response_failed(struct HttpServe*) {
  g_approved = false;
  g_rx_active = false;
  strlcpy(g_img_errmsg, "response write failed", sizeof(g_img_errmsg));
  g_img_err++;
}
static void expire_approval() {
  if (!g_approved || millis() - g_approved_at < 90000) return;
  g_approved = false;
  g_rx_active = false;
  strlcpy(g_img_errmsg, "no upload after acceptance", sizeof(g_img_errmsg));
  g_img_err++;
}
static bool sv_authorized(struct HttpServe*) {
  expire_approval();
  uint8_t peer[16];
  if (!g_approved || !peer_address(peer) || memcmp(peer, g_approved_peer, 16) != 0) return false;
  g_approved = false;
  return true;
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

static void serve_http(WOLFSSL* ssl) {
  struct HttpRdr r;
  http_rdr_init(&r, ssl, http_src_tls);
  struct HttpServe sv;
  memset(&sv, 0, sizeof(sv));
  sv.r = &r;
  sv.io = ssl;
  sv.write = sv_write;
  sv.now = sv_now;
  sv.log = nullptr;
  sv.set_timeout = http_set_timeout;
  sv.disc_body = AD_DISC_BODY;
  sv.disc_len = (int)sizeof(AD_DISC_BODY);
  sv.ask_body = AD_ASK_BODY;
  sv.ask_len = (int)sizeof(AD_ASK_BODY);
  sv.disc_ms = &g_disc_ms;
  sv.ask_ms = &g_ask_ms;
  sv.disc_ok = &g_discover_ok;
  sv.ask_ok = &g_ask_ok;
  sv.desync = &g_desync;
  sv.on_ask_sent = sv_on_ask_sent;
  sv.response_failed = sv_response_failed;
  sv.upload = sv_upload;
  sv.peer_waiting = sv_peer_waiting;
  sv.prepare_ask = sv_prepare_ask;
  sv.cancelled = sv_cancelled;
  sv.approve = sv_approve;
  sv.authorized = sv_authorized;
  serve_http_run(&sv);
}

// TLS acceptor as a single-task, single-session, non-blocking select() loop that
// "follows the ClientHello". sharingd opens ~4 PARALLEL connections (racing) and
// sends its real ClientHello on the one it commits to; a blocking one-at-a-time
// server was always stuck waiting on a dead connection while the live CH rotted
// unread. Here every fd is O_NONBLOCK; we select() over all of them, adopt the
// NEWEST fd that has a queued ClientHello (MSG_PEEK), step the handshake without
// blocking, and preempt a silent active connection if a newer one gets a CH.
#define MAXC 12

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
    expire_approval();
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
    for (;;) {
      struct sockaddr_in6 c;
      socklen_t cl = sizeof(c);
      int fd = accept(ls, (struct sockaddr*)&c, &cl);
      if (fd < 0) break;
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      set_nonblock(fd, true);
      g_tcp_accept++;

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
        close(fd);
      }
    }

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
      close(conn[active]);
      conn[active] = -1;
      active = -1;
      tls_reset_session();
      adopt = true;
    }

    if (adopt) {
      g_tls_fd = conn[cand];
      if (!g_ssl) g_ssl = wolfSSL_new(g_tlsconf);
      if (!g_ssl) {
        close(conn[cand]);
        conn[cand] = -1;
        g_tls_fd = -1;
        continue;
      }
      active = cand;
      t_hs0 = t_data = now;
    }

    // 4. Step the handshake on the active connection (non-blocking).
    if (active >= 0 && conn[active] >= 0) {
      if (FD_ISSET(conn[active], &rf)) t_data = now;
      int r = wolfSSL_accept(g_ssl);
      if (r == WOLFSSL_SUCCESS) {
        g_tls_ok++;

        int afd = conn[active];
        for (int i = 0; i < MAXC; i++)
          if (conn[i] >= 0 && i != active) {
            close(conn[i]);
            conn[i] = -1;
          }
        set_nonblock(afd, false);  // HTTP phase: blocking + read timeout
        set_rd_timeout(10000);

        serve_http(g_ssl);

        wolfSSL_shutdown(g_ssl);
        close(afd);
        conn[active] = -1;
        active = -1;
        tls_reset_session();
      } else if (!tls_want_io(wolfSSL_get_error(g_ssl, r))) {
        g_tls_failures.fetch_add(1);
        g_last_tls_error.store(wolfSSL_get_error(g_ssl, r));
        close(conn[active]);
        conn[active] = -1;
        active = -1;
        tls_reset_session();
      }
    } else
      active = -1;

    // 5. Handshake deadline: drop a connection that never finishes.
    if (active >= 0 && (uint32_t)(now - t_hs0) > 15000) {
      g_tls_failures.fetch_add(1);
      close(conn[active]);
      conn[active] = -1;
      active = -1;
      tls_reset_session();
    }
  }
  for (int i = 0; i < MAXC; i++)
    if (conn[i] >= 0) close(conn[i]);
  if (ls >= 0) close(ls);
  g_listen_fd = -1;
  g_tls_fd = -1;
  airdrop_tls_release();
  g_listener_running.store(false);
  vTaskDeleteWithCaps(nullptr);
}

/* ==========================================================================
 * The seam. Everything above this line moved unchanged; everything below it is
 * new, and it is new because twenty file-scope globals used to cross here.
 * ========================================================================== */

/* --- the received file ---------------------------------------------------- */

void ad_poll(void) {
  auto pending = StreamSlot::Pending;
  if (g_stream_slot.compare_exchange_strong(pending, StreamSlot::Processing)) {
    g_stream_result = g_stream_cb && g_stream_cb(g_stream_ctx, g_stream_event, g_stream_data, g_stream_len);
    g_stream_slot.store(StreamSlot::Idle);
  }
}

/* --- lifecycle ------------------------------------------------------------ */

const char* ad_receiver_name() { return AD_RECEIVER_NAME; }

int ad_begin(uint32_t max_receive) {
  if (g_listener_running.load() || !g_stream_cb) return -1;
  g_stop.store(false);
  g_consent.store(Consent::Idle);
  g_approved = false;

  g_rx_active = false;
  g_rx_bytes = 0;
  g_socket_rx_bytes = 0;
  g_tls_failures = 0;
  g_last_tls_error = 0;
  g_img_errmsg[0] = 0;

  // Limit total extracted content; the streaming buffers stay bounded independently.
  g_rx_max = max_receive ? max_receive : RX_MAX_AUTO;

  /* AWDL must already be up: the identity below is derived from the interface MAC, and
     the mDNS records we are about to advertise are built out of it. */
  awdl_identity(&g_id);
  awdl_rx_tap_set(ad_rx_tap, nullptr);
  /* No announce callback is registered -- there is no periodic multicast announcement, for
     the reasons set out in the note above ad_respond(). The hook itself stays available. */
  // The listener performs no flash writes; its 32 KiB stack can use PSRAM.
  // TLS uses the existing wolfSSL allocator; only this task stack is forced to PSRAM.
  g_listener_running.store(true);
  if (xTaskCreatePinnedToCoreWithCaps(tcp_listen_task, "tcp_listen", 32768, nullptr, 2, nullptr, 1,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    g_listener_running.store(false);
    return -1;
  }
  return 0;
}

bool ad_end() {
  g_stop.store(true);
  ad_request_answer(false);
  if (g_listener_running.load() && g_tls_fd >= 0) shutdown(g_tls_fd, SHUT_RDWR);
  const uint32_t start = millis();
  while (g_listener_running.load() && millis() - start < 7000) vTaskDelay(pdMS_TO_TICKS(10));
  if (g_listener_running.load()) return false;
  airdrop_tls_release();
  awdl_rx_tap_set(nullptr, nullptr);
  g_rx_active = false;
  g_approved = false;
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
  out->awaiting_upload = g_approved.load();
  out->upload_pending = g_upload_pending.load();
  out->socket_rx_bytes = g_socket_rx_bytes.load();
  out->tls_failures = g_tls_failures.load();
  out->last_tls_error = g_last_tls_error.load();
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
  out->tcp_tous = g_tcp_tous;
  out->syn_airdrop = g_syn_airdrop;
  out->syn_pair1 = g_syn_pair1;
  out->syn_pair2 = g_syn_pair2;
  out->syn_other = g_syn_other;
  out->syn_other_port = g_syn_other_port;
}
