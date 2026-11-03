/* awdl_frame.h -- build an AWDL Master Indication Frame.
 *
 * This is the byte-for-byte frame that makes an Apple device accept us as a peer, and
 * it was already written as pure code: 178 lines with exactly two platform calls, both
 * of them clock reads, and four globals it happened to reach for instead of taking as
 * arguments. Extracting it was argument-passing, not porting -- the translation from
 * the firmware's build_mif was done by script and its diff is nothing but parameter
 * renames, so the bytes cannot have moved. tools/test-frame.sh proves that: it holds
 * golden vectors produced by the pre-extraction implementation and requires this one to
 * reproduce them exactly.
 *
 * WHY THIS FRAME IS DELICATE
 *
 * Per OWL's rx.c a peer only becomes connectable once it has seen a MIF *and* a valid
 * version/devclass, so every TLV in the chain has to parse. The TLV lengths here are
 * HAND-WRITTEN LITERALS: a wrong one does not fail loudly, it shifts every following
 * TLV and the peer silently stops reading the chain. That has happened. Three lengths
 * move together whenever the TXT string changes (the TLV length, the record data_len,
 * and the label length), and tools/frame_test.c walks the chain the way a peer does and
 * says where it breaks. Run tools/test-frame.sh after touching anything here.
 *
 * Two things in the payload are not arbitrary and were paid for:
 *
 *   - We advertise ourselves as a LEAF (distance_to_master = 1) with a deliberately
 *     weak self_metric, echoing the elected root's real address, metric and counter.
 *     An earlier frame claimed distancetop = 0 -- "I am the root" -- while pointing
 *     top_master at a foreign MAC with a counter frozen at 0; iOS read that as
 *     out-of-mesh and never peered.
 *   - The service_response TLVs announce _airdrop._tcp AT THE AWDL LAYER. iOS builds
 *     its AirDrop candidate list from the action frame, not only from mDNS. Their name
 *     compression is a fixed dictionary (C0 07 = _airdrop._tcp.local, C0 0C = local,
 *     C0 00 = root) validated byte-for-byte against a real Mac's on-air TLVs. That
 *     validation covers _airdrop and nothing else, so this encoder is deliberately
 *     specific rather than a general DNS-SD-over-AWDL writer.
 *
 * Dependency-free integer C: it compiles unchanged into the firmware and into
 * tools/test-frame.sh.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "awdl_window.h"

/* Buffer floor the caller must provide. The builder emits a variable-length frame and
 * does NOT bounds-check each of its ~180 writes -- that would be 180 branches guarding
 * against a caller who passed a buffer too small for a frame whose size is known in
 * advance. Requiring one generous minimum is the honest contract instead. Measured
 * output is 401 bytes for a 12-character instance; the arpa TLV is the only
 * instance-length-sensitive field and it clamps at 20, so the true worst case is 409.
 */
#define AWDL_MIF_MAX 512

struct AwdlMifParams {
  const uint8_t* src;      /* our AWDL MAC: addr2, and data_path_state's awdl_addr */
  const uint8_t* master;   /* the ELECTED ROOT: master_addr / top_master_addr /
                              sync_addr. Never our own address -- advertising
                              ourselves as root is what made iOS refuse to peer,
                              and a peer echoing it back seeds a self-election. */
  const char* instance;    /* 12-hex service instance and host label. EXACTLY 12
                              characters: the service_response TLVs write it with
                              a hand-written length byte of 0x0C. */
  uint32_t tsf_us;         /* phy_tx and target_tx: our clock at the instant the
                              frame goes on air. Receivers pair their rx timestamp
                              with phy_tx, so this is what their phase estimate
                              measures -- fill it as late as possible. */
  uint32_t metric;         /* the root's top_master_metric, echoed. 0 -> 60 */
  uint32_t master_counter; /* the root's master_counter, echoed. A counter frozen
                              at 0 reads as out-of-mesh. */

  /* Live timing, written over the skeleton at fixed sync-relative offsets. When
     have_window is false these fields are left at zero, which is what an unsynced
     sender should say. The firmware never transmits in that state -- TX is gated on
     having an elected master -- but the builder does not assume its caller's gate. */
  bool have_window;
  uint32_t rx_now_us; /* now, in the RX timestamp timebase */
  uint32_t phase_us;  /* the window phase in that same timebase */
  uint16_t cur_aw_seq;
};

static void awdl_put32le(uint8_t* d, uint32_t v) {
  d[0] = (uint8_t)v;
  d[1] = (uint8_t)(v >> 8);
  d[2] = (uint8_t)(v >> 16);
  d[3] = (uint8_t)(v >> 24);
}

/* Returns the frame length, or 0 if cap < AWDL_MIF_MAX. */
static size_t awdl_build_mif(uint8_t* b, size_t cap, const struct AwdlMifParams* m) {
  if (cap < AWDL_MIF_MAX) return 0;
  size_t p = 0;
  // ---- 802.11 mgmt action header (24) ----
  b[p++] = 0xd0;
  b[p++] = 0x00;
  b[p++] = 0x00;
  b[p++] = 0x00;
  memset(&b[p], 0xff, 6);
  p += 6;  // addr1 broadcast
  memcpy(&b[p], m->src, 6);
  p += 6;  // addr2 = us
  memcpy(&b[p], AWDL_BSSID, 6);
  p += 6;  // addr3 = AWDL bssid
  b[p++] = 0x00;
  b[p++] = 0x00;  // seq_ctrl
  // ---- awdl_action (16), subtype = MIF ----
  memcpy(&b[p], APPLE_VENDOR, 4);
  p += 4;  // category(0x7f)+oui
  b[p++] = 0x08;
  b[p++] = 0x10;
  b[p++] = 0x03;
  b[p++] = 0x00;  // type, ver=1.0, subtype=3, reserved
  awdl_put32le(&b[p], m->tsf_us);
  p += 4;  // phy_tx
  awdl_put32le(&b[p], m->tsf_us);
  p += 4;  // target_tx

  const size_t sync_off = p;  // = 40; live-overwrite base
  // ---- sync_params (type 4) content=73 ----
  b[p++] = 0x04;
  b[p++] = 0x49;
  b[p++] = 0x00;
  b[p++] = 0x06;  // next_aw_channel
  b[p++] = 0x00;
  b[p++] = 0x00;  // tx_down_counter (live-ish)
  b[p++] = 0x06;  // master_channel
  b[p++] = 0x00;  // guard_time
  b[p++] = 0x10;
  b[p++] = 0x00;  // aw_period=16
  b[p++] = 0x6e;
  b[p++] = 0x00;  // af_period=110
  b[p++] = 0x00;
  b[p++] = 0x18;  // flags=0x1800
  b[p++] = 0x10;
  b[p++] = 0x00;  // aw_ext_length=16
  b[p++] = 0x10;
  b[p++] = 0x00;  // aw_com_length=16
  b[p++] = 0x00;
  b[p++] = 0x00;  // remaining_aw_length (live @sync_off+18)
  b[p++] = 0x03;
  b[p++] = 0x03;
  b[p++] = 0x03;
  b[p++] = 0x03;  // presence exts (mode 4)
  memcpy(&b[p], m->master, 6);
  p += 6;         // master_addr
  b[p++] = 0x04;  // presence_mode=4
  b[p++] = 0x00;  // reserved
  b[p++] = 0x00;
  b[p++] = 0x00;  // next_aw_seq (live @sync_off+32)
  b[p++] = 0x00;
  b[p++] = 0x00;  // ap_alignment
  b[p++] = 0x0f;
  b[p++] = 0x03;
  b[p++] = 0x00;
  b[p++] = 0x03;
  b[p++] = 0xff;
  b[p++] = 0xff;  // chanseq hdr
  for (int i = 0; i < 16; i++) {
    b[p++] = 0x06;
    b[p++] = 0x51;
  }  // 16x ch6 (opclass)
  b[p++] = 0x00;
  b[p++] = 0x00;  // pad

  // Coherent leaf election: we advertise ourselves at distance 1 under the REAL
  // elected root (=master, s.addr), echoing its true metric + counter, with a
  // weak self_metric so we never claim to be the master. (The old frame said
  // distancetop=0 "I am root" while pointing top_master at a foreign MAC, with
  // counter frozen at 0 -- iOS read that as out-of-mesh and never peered.)
  uint32_t el_metric = m->metric ? m->metric : 60;  // root's top_master_metric (echo)
  uint32_t el_counter = m->master_counter;          // root's master_counter (echo)
  const uint8_t self_metric = 0x3c;                 // 60 = weak; always < a real root
  // ---- election_params v1 (type 5) content=21 ----
  b[p++] = 0x05;
  b[p++] = 0x15;
  b[p++] = 0x00;
  b[p++] = 0x00;  // flags
  b[p++] = 0x00;
  b[p++] = 0x00;  // id
  b[p++] = 0x01;  // distancetop = 1 (leaf under root)
  b[p++] = 0x00;  // unknown
  memcpy(&b[p], m->master, 6);
  p += 6;  // top_master_addr = elected root
  b[p++] = el_metric & 0xff;
  b[p++] = (el_metric >> 8) & 0xff;
  b[p++] = (el_metric >> 16) & 0xff;
  b[p++] = (el_metric >> 24) & 0xff;  // top_master_metric
  b[p++] = self_metric;
  b[p++] = 0x00;
  b[p++] = 0x00;
  b[p++] = 0x00;  // self_metric (weak)
  b[p++] = 0x00;
  b[p++] = 0x00;  // pad

  // ---- chanseq (type 18) content=41 ----
  b[p++] = 0x12;
  b[p++] = 0x29;
  b[p++] = 0x00;
  b[p++] = 0x0f;
  b[p++] = 0x03;
  b[p++] = 0x00;
  b[p++] = 0x03;
  b[p++] = 0xff;
  b[p++] = 0xff;
  for (int i = 0; i < 16; i++) {
    b[p++] = 0x06;
    b[p++] = 0x51;
  }
  b[p++] = 0x00;
  b[p++] = 0x00;
  b[p++] = 0x00;  // pad(3)

  // ---- election_params v2 (type 24) content=40 ----
  b[p++] = 0x18;
  b[p++] = 0x28;
  b[p++] = 0x00;
  memcpy(&b[p], m->master, 6);
  p += 6;  // master_addr = elected root
  memcpy(&b[p], m->master, 6);
  p += 6;  // sync_addr = root (we sync directly to it)
  b[p++] = el_counter & 0xff;
  b[p++] = (el_counter >> 8) & 0xff;
  b[p++] = (el_counter >> 16) & 0xff;
  b[p++] = (el_counter >> 24) & 0xff;  // master_counter (echo)
  b[p++] = 0x01;
  b[p++] = 0x00;
  b[p++] = 0x00;
  b[p++] = 0x00;  // distance_to_master = 1
  b[p++] = el_metric & 0xff;
  b[p++] = (el_metric >> 8) & 0xff;
  b[p++] = (el_metric >> 16) & 0xff;
  b[p++] = (el_metric >> 24) & 0xff;  // master_metric (echo)
  b[p++] = self_metric;
  b[p++] = 0x00;
  b[p++] = 0x00;
  b[p++] = 0x00;  // self_metric (weak)
  memset(&b[p], 0, 4);
  p += 4;  // unknown
  memset(&b[p], 0, 4);
  p += 4;  // reserved
  memset(&b[p], 0, 4);
  p += 4;  // self_counter = 0 (we are not a master)

  // ---- service_params (type 6) content=9 ----
  b[p++] = 0x06;
  b[p++] = 0x09;
  b[p++] = 0x00;
  memset(&b[p], 0, 9);
  p += 9;

  // ---- service_response (type 2) x3: announce our _airdrop._tcp AS AN AWDL PEER
  // (E3). This is the layer we were missing: we advertised _airdrop only via the
  // mDNS DATA frame, but Apple peers announce their DNS-SD services INSIDE the
  // action frame here, and iOS builds its AirDrop-peer candidate list from it.
  // AWDL name compression = fixed dictionary: _airdrop._tcp.local=C0 07,
  // local=C0 0C, root=C0 00. Record = name_len(LE)|name|type(1B)|data_len(LE)|
  // class(2B)|rdata. VALIDATED byte-for-byte against a real Mac's on-air type-2
  // TLVs we sniffed on Device2 (class=00 00 confirmed; PTR rdata ends C0 00 like
  // the real device, NOT C0 07). SRV target = <instance>.local (a host we DO
  // answer AAAA for), unlike the Mac's UUID.local. (Wireshark packet-awdl.c was
  // read-only reference; nothing vendored -- org policy.)
  // PTR: _airdrop._tcp.local -> <instance>
  b[p++] = 0x02;
  b[p++] = 0x18;
  b[p++] = 0x00;  // type2, TLV len=24 (LE)
  b[p++] = 0x03;
  b[p++] = 0x00;  // name_len=3 (2 name +1 type)
  b[p++] = 0xC0;
  b[p++] = 0x07;  // owner=_airdrop._tcp.local
  b[p++] = 0x0C;  // PTR
  b[p++] = 0x0F;
  b[p++] = 0x00;  // data_len=15
  b[p++] = 0x00;
  b[p++] = 0x00;  // class (real=00 00)
  b[p++] = 0x0C;
  memcpy(&b[p], m->instance, 12);
  p += 12;  // <instance>
  b[p++] = 0xC0;
  b[p++] = 0x00;  // root (mimic real Mac)
  // SRV: <instance>._airdrop._tcp.local -> port 8770, target <instance>.local
  b[p++] = 0x02;
  b[p++] = 0x2B;
  b[p++] = 0x00;  // type2, TLV len=43 (LE)
  b[p++] = 0x10;
  b[p++] = 0x00;  // name_len=16
  b[p++] = 0x0C;
  memcpy(&b[p], m->instance, 12);
  p += 12;
  b[p++] = 0xC0;
  b[p++] = 0x07;
  b[p++] = 0x21;  // SRV
  b[p++] = 0x15;
  b[p++] = 0x00;  // data_len=21
  b[p++] = 0x00;
  b[p++] = 0x00;  // class
  b[p++] = 0x00;
  b[p++] = 0x00;  // priority (BE)
  b[p++] = 0x00;
  b[p++] = 0x00;  // weight (BE)
  b[p++] = 0x22;
  b[p++] = 0x42;  // port 8770 (BE)
  b[p++] = 0x0C;
  memcpy(&b[p], m->instance, 12);
  p += 12;
  b[p++] = 0xC0;
  b[p++] = 0x0C;  // target <instance>.local
  // TXT: flags=1019 -- the SAME value the mDNS TXT publishes.
  // It used to say flags=111611 here, under a comment claiming the two matched. They
  // did not: 111611 = 0x1B3FB = 1019 (0x3FB) with four spurious high bits (12,13,15,
  // 16) set: 111611 was the garbage value that 1019 had already replaced elsewhere
  // -- the replacement just never reached this path. So one DNS-SD instance
  // name was publishing two different TXT rdata for the same unique record, and the
  // wrong one about ten times as often (a MIF every ~250ms versus an mDNS announce
  // every 2.5s). The comment asserting they matched is the kind of thing this project
  // has learned to distrust: it was checked, and it was false.
  // Note the three hand-written lengths that all shrink by 2 with the string:
  // TLV len 35->33, data_len 13->11, and the label length 0x0C->0x0A. Verified by
  // walking the firmware's own MIFHEX dump.
  b[p++] = 0x02;
  b[p++] = 0x21;
  b[p++] = 0x00;  // type2, TLV len=33 (LE)
  b[p++] = 0x10;
  b[p++] = 0x00;  // name_len=16
  b[p++] = 0x0C;
  memcpy(&b[p], m->instance, 12);
  p += 12;
  b[p++] = 0xC0;
  b[p++] = 0x07;
  b[p++] = 0x10;  // TXT
  b[p++] = 0x0B;
  b[p++] = 0x00;  // data_len=11
  b[p++] = 0x00;
  b[p++] = 0x00;  // class
  b[p++] = 0x0A;
  memcpy(&b[p], "flags=1019", 10);
  p += 10;

  // ---- ht_capabilities (type 7) content=8 ----
  b[p++] = 0x07;
  b[p++] = 0x08;
  b[p++] = 0x00;
  b[p++] = 0x00;
  b[p++] = 0x00;  // unknown
  b[p++] = 0xce;
  b[p++] = 0x11;  // ht_capabilities=0x11ce
  b[p++] = 0x1b;  // ampdu_params
  b[p++] = 0xff;  // rx_mcs
  b[p++] = 0x00;
  b[p++] = 0x00;  // unknown2

  // ---- arpa (type 16) content=1+1+len+2 ----
  size_t hn = strlen(m->instance);
  if (hn > 20) hn = 20;  // guard (buffer/label sanity)
  uint16_t arpa_len = (uint16_t)(1 + 1 + hn + 2);
  b[p++] = 0x10;
  b[p++] = arpa_len & 0xff;
  b[p++] = (arpa_len >> 8) & 0xff;
  b[p++] = 0x03;         // flags=3
  b[p++] = (uint8_t)hn;  // name_length
  memcpy(&b[p], m->instance, hn);
  p += hn;  // name
  b[p++] = 0xc0;
  b[p++] = 0x0c;  // .local suffix pointer

  // ---- data_path_state (type 12) content=15 ----
  b[p++] = 0x0c;
  b[p++] = 0x0f;
  b[p++] = 0x00;
  b[p++] = 0x24;
  b[p++] = 0x8f;  // flags=0x8f24
  b[p++] = 'X';
  b[p++] = '0';
  b[p++] = 0x00;  // country_code
  b[p++] = 0x01;
  b[p++] = 0x00;  // social_channels = ch6 bit
  memcpy(&b[p], m->src, 6);
  p += 6;  // awdl_addr = OUR mac (EUI-64 neighbour)
  b[p++] = 0x00;
  b[p++] = 0x00;  // ext_flags

  // ---- version (type 21) content=2 ----
  // Was 0x34 (AWDL 3.4, an OWL-era value from ~2018). Device2 measured the real
  // Apple peers around us at 0x93 (9.3) and 0xa0 (10.0); ours was the outlier and
  // iOS likely refuses to peer with a v3.4 node (OpenDrop #80). Bumped to 9.3 to
  // match our sync master (f204..). devclass 0x01 already matches all real peers.
  b[p++] = 0x15;
  b[p++] = 0x02;
  b[p++] = 0x00;
  b[p++] = 0x93;  // version 9.3 (match current mesh)
  b[p++] = 0x01;  // devclass = macOS (matches real peers)

  // ---- live timing overwrite (offsets relative to the sync TLV) ----
  if (m->have_window) {
    uint32_t rx_now = m->rx_now_us;
    int64_t position = (((int64_t)rx_now - (int64_t)m->phase_us) % AWC_US + AWC_US) % AWC_US;
    int64_t rem_us = AW_US - (position % AW_US);
    uint16_t rem_tu = (uint16_t)(rem_us / TU_US);
    if (rem_tu > 16) rem_tu = 16;
    b[sync_off + 18] = rem_tu & 0xff;
    b[sync_off + 19] = (rem_tu >> 8) & 0xff;
    b[sync_off + 32] = m->cur_aw_seq & 0xff;
    b[sync_off + 33] = (m->cur_aw_seq >> 8) & 0xff;
  }
  return p;
}

/* ===========================================================================
 * PARSING
 * ---------------------------------------------------------------------------
 * The receive half. These read the frames the mesh sends us, and everything they
 * produce feeds either the phase gauge or the master table -- and the master table
 * drives the election, which is where this project's two worst outages came from.
 * Both were found by a human noticing the badge had vanished from AirDrop. So the
 * decoders are pure functions with fixtures, like awdl_elect.h, rather than code that
 * can only be exercised on the device.
 *
 * ONE BEHAVIOUR CHANGE from the firmware they replace, deliberate: the sync-params
 * decoder checks its length. handle_sync() read val[21..26] as the master address and
 * val[29..30] as the AW sequence with NO length guard at all, while the election-v1,
 * election-v2 and chanseq handlers next to it all guarded theirs. The TLV walk bounds
 * the read to the frame buffer, so it was never a crash -- but a peer emitting a short
 * sync TLV would have had six bytes of whatever followed registered as a master
 * address, in the table the election chooses from. Now it is rejected.
 */

struct AwdlTlv {
  uint8_t tag;
  uint16_t len;
  const uint8_t* val;
};

/* The 802.11 + AWDL action-frame header, to the depth we actually read it.
 * Layout, confirmed against the builder above and against captured frames:
 *   pl[0]      0xd0, management/action
 *   pl[4..9]   addr1 (dst)      pl[10..15] addr2 (src)   pl[16..21] addr3 (bssid)
 *   pl[24..27] Apple vendor     pl[30]     subtype: 0 = PSF, 3 = MIF
 *   pl[32..35] phy_tx           pl[36..39] target_tx     pl[40..]   the TLV chain
 *
 * phy_tx is the sender's TSF at the instant the frame hit the air -- the same instant
 * our rx timestamp measures. Pairing rx with phy_tx gives 31-43us of residual scatter;
 * pairing it with target_tx gives 915-1257us with a +5ms tail, because that tail IS the
 * medium-access delay. Do not "improve" the estimator by using target_tx.
 */
struct AwdlActionHdr {
  const uint8_t *dst, *src, *bssid;
  uint8_t subtype;
  uint32_t phy_tx, tgt_tx;
  const uint8_t *tlv, *tlv_end;
};

#define AWDL_ACTION_HDR_LEN 40
#define AWDL_SUBTYPE_PSF 0
#define AWDL_SUBTYPE_MIF 3

static uint32_t awdl_get32le(const uint8_t* d) {
  return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}
static uint16_t awdl_get16le(const uint8_t* d) { return (uint16_t)((uint16_t)d[0] | ((uint16_t)d[1] << 8)); }

/* True if this is an AWDL action frame and the header is complete. */
static bool awdl_parse_action(const uint8_t* pl, size_t len, struct AwdlActionHdr* o) {
  if (len < AWDL_ACTION_HDR_LEN) return false;
  if (pl[0] != 0xd0) return false;
  if (memcmp(&pl[24], APPLE_VENDOR, 4) != 0) return false;
  o->dst = &pl[4];
  o->src = &pl[10];
  o->bssid = &pl[16];
  o->subtype = pl[30];
  o->phy_tx = awdl_get32le(&pl[32]);
  o->tgt_tx = awdl_get32le(&pl[36]);
  o->tlv = &pl[AWDL_ACTION_HDR_LEN];
  o->tlv_end = pl + len;
  return true;
}

/* Step one TLV. Returns false at the end of the chain OR on a length that would run
   past it -- a truncated chain and a complete one are not distinguished, because a
   peer stops parsing at the first bad length either way. */
static bool awdl_tlv_next(const uint8_t** cur, const uint8_t* end, struct AwdlTlv* o) {
  const uint8_t* t = *cur;
  if (t + 3 > end) return false;
  uint16_t len = awdl_get16le(&t[1]);
  if (t + 3 + len > end) return false;
  o->tag = t[0];
  o->len = len;
  o->val = t + 3;
  *cur = t + 3 + len;
  return true;
}

/* --- sync_params (tag 4) --------------------------------------------------- */
struct AwdlSyncParams {
  const uint8_t* master; /* the sync master this sender announces */
  uint16_t aw_seq;       /* next_aw_seq, as announced (raw, not +1) */
  uint16_t remaining_tu; /* remaining_aw_length. Kept for diagnostics only: the
                            estimator does not use it, and a capture measured
                            12.9% of real frames carrying a NON-zero value, so the
                            old comment claiming "always 0" was wrong. */
};
#define AWDL_SYNC_MIN_LEN 31 /* master at [21..26], aw_seq at [29..30] */
static bool awdl_parse_sync(const struct AwdlTlv* t, struct AwdlSyncParams* o) {
  if (t->tag != TLV_SYNC_PARAMS || t->len < AWDL_SYNC_MIN_LEN) return false;
  o->master = &t->val[21];
  o->remaining_tu = awdl_get16le(&t->val[15]);
  o->aw_seq = awdl_get16le(&t->val[29]);
  return true;
}

/* --- election params (tags 5 and 24) --------------------------------------- */
/* Both are keyed by the ROOT of the sync tree, not by the sender. v2 additionally
   carries master_counter, the freshness counter iOS tracks -- echoing the root's real
   counter is part of looking in-mesh; a counter frozen at 0 reads as out-of-mesh. */
struct AwdlElection {
  const uint8_t* master;
  uint32_t metric;
  uint32_t counter;
  bool have_counter; /* false for v1, which has no counter field */
};
static bool awdl_parse_election_v1(const struct AwdlTlv* t, struct AwdlElection* o) {
  if (t->tag != TLV_ELECTION_PARAMS || t->len < 19) return false;
  o->master = &t->val[5];                /* top_master_addr */
  o->metric = awdl_get32le(&t->val[11]); /* top_master_metric */
  o->counter = 0;
  o->have_counter = false;
  return true;
}
static bool awdl_parse_election_v2(const struct AwdlTlv* t, struct AwdlElection* o) {
  if (t->tag != TLV_ELECTION_V2 || t->len < 28) return false;
  o->master = &t->val[0];                 /* master_addr */
  o->counter = awdl_get32le(&t->val[12]); /* master_counter */
  o->metric = awdl_get32le(&t->val[20]);  /* master_metric */
  o->have_counter = true;
  return true;
}

/* --- channel sequence (tag 18) --------------------------------------------- */
/* 16 slots of 4 AW each across the AWC. The slots reading 6 are the ch6 dwell, and
   they are the only time a peer can hear us: this is what the TX gate is built from.
   The entries are 2 bytes wide and only the first is the channel. */
struct AwdlChanSeq {
  uint8_t slot[16];
};
static bool awdl_parse_chanseq(const struct AwdlTlv* t, struct AwdlChanSeq* o) {
  if (t->tag != TLV_CHAN_SEQ || t->len < 6 + 32) return false;
  for (int i = 0; i < 16; i++) o->slot[i] = t->val[6 + i * 2];
  return true;
}

/* Bitmask of every ch6 slot. The mesh densifies ch6 under load -- 1/16 to 5/16 has
   been observed during a transfer -- so gating TX to only the first one throws that
   extra airtime away. */
static uint16_t awdl_chanseq_ch6_mask(const struct AwdlChanSeq* c) {
  uint16_t m = 0;
  for (int i = 0; i < 16; i++)
    if (c->slot[i] == 6) m |= (uint16_t)(1u << i);
  return m;
}
