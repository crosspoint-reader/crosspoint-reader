#include <freertos/idf_additions.h>
/* awdl_port_esp32.cpp -- Apple Wireless Direct Link on the ESP32 radio.
 *
 * The frame path, the mesh phase estimator, master election, the transmit cadence, the
 * raw 802.11 injection sites and the lwIP netif that rides on them. The reasoning is
 * attached to the code because every non-obvious line in this file was paid for with a
 * capture, and two of them were paid for with a badge that vanished from AirDrop for
 * 74 seconds.
 *
 * WHAT IS NOT HERE, and where it went. All of it is dependency-free C with a host suite,
 * which is the point of the split -- the parts that can be wrong in a way you can test on
 * a laptop are the parts that were wrong:
 *   awdl_gauge.h   the mesh-wide phase gauge      awdl_elect.h   the election decision
 *   awdl_window.h  AW/AWC arithmetic              awdl_tick.h    the cadence schedule
 *   awdl_peer.h    the master table and the PLL   awdl_frame.h   TLV parse and MIF build
 *   awdl_ring.h    the SPSC cursor                awdl_src.h     per-sender residuals
 *
 * THREE INVARIANTS. They are not style; each has a measured failure behind it.
 *   (a) NOTHING ON THE FRAME PATH MAY BLOCK. No Serial, no lwIP, no waiting. A 958-byte
 *       hex dump written directly from process_frame cost 21 ms -- longer than one AW --
 *       and hid for months because it was capped per boot.
 *   (b) TX IS GATED ON AN ELECTED MASTER. No master means no transmission, by design,
 *       reported rather than worked around.
 *   (c) ONE WRITER OF THE TRANSMIT GATE. The published window and selection are written
 *       only by awdl_elect_and_publish, under one critical section, and read by copy.
 *
 * A NOTE ON ARDUINO. In a .ino the build generates prototypes for every function and
 * inserts them above the body, so definition order does not matter. A .cpp has no such
 * thing, which is why this file opens with forward declarations that look redundant and
 * are not: without them, six calls and one function-pointer reference do not compile.
 */
#include <Arduino.h> /* millis(); the .ino got this via M5Unified for free */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/awdl_ack.h"
#include "../core/awdl_elect.h"
#include "../core/awdl_frame.h"
#include "../core/awdl_gauge.h"
#include "../core/awdl_lru.h"
#include "../core/awdl_peer.h"
#include "../core/awdl_ring.h"
#include "../core/awdl_select.h"
#include "../core/awdl_src.h"
#include "../core/awdl_tick.h"
#include "../core/awdl_window.h"
#include "awdl_port_esp32.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
/* The promiscuous callback's frame snapshot. SNAP_LEN must hold a FULL frame: an AWDL
 * data frame carrying a TLS ClientHello is ~1580 B. Truncate one and lwIP is handed an
 * incomplete TCP segment and drops it -- while a SYN still fits, so the connection is
 * accepted and the socket then never becomes readable, which reads as a peer that dialled
 * in and said nothing. */
#define RING_SLOTS 16
#define SNAP_LEN 1600
struct RawFrame {
  uint32_t rx_ts; /* hardware rx timestamp (us)                            */
  int64_t now_us; /* esp_timer at callback entry, for TX-window conversion */
  int8_t rssi;
  uint16_t len;
  uint8_t data[SNAP_LEN];
};

/* Forward declarations -- see the note at the top of this file. Six calls precede their
 * definitions, and awdl_cadence_task_fn is handed to xTaskCreatePinnedToCore before it
 * exists. In the .ino none of this was visible, which is exactly why it is written out. */
static uint16_t ch6_mask_measured(int* peak);
static uint16_t ch6_mask_of(const uint8_t* addr);
static int ch6_slot_of(const uint8_t* addr);
static bool in_ch6_window_now(void);
static void ignore_state(esp_err_t e);
static esp_err_t wifi_probe_init(void);
static void awdl_netif_flush_tx(int budget, int64_t deadline_us);
static esp_err_t awdl_netif_init(void);
static void awdl_cadence_task_fn(void*);
static void proc_task(void*);
static bool awdl_win_phase(const struct AwdlWinState* w, int64_t* out);
/* The RX tap's state. Defined with the rest of the seam near the bottom; declared here
   because handle_data reads it a thousand lines earlier. */
/* True only while a callback the port itself invoked is running -- the RX tap, or the
   announce hook. awdl_tx_ip6_now() refuses outside those windows.
   VOLATILE, and not defensively: this is a file-static whose address is never taken, and
   the callback that reads it (through awdl_tx_ip6_now) is reached by an INDIRECT call
   into another translation unit. A compiler is entitled to conclude that nothing between
   the two stores can observe it and delete them both. volatile removes the question for
   free; the alternative is a permission that works at -O0 and not at -Os. */
static volatile bool g_in_tap = false;
static awdl_rx_tap_fn g_rx_tap = nullptr;
static void* g_rx_tap_ctx = nullptr;
static int build_awdl_data(uint8_t* out, const uint8_t* dst_mac, uint16_t ethertype, const uint8_t* ip, int iplen,
                           uint16_t seq);
static void build_awdl_data_at(uint8_t* buf, const uint8_t* dst_mac, uint16_t ethertype, int iplen, uint16_t seq);

// ---- raw injection: no unlock needed, and here is why -----------------------
// esp_wifi_80211_tx runs every frame through ieee80211_raw_frame_sanity_check() in
// libnet80211.a. The usual workaround is to override that symbol to return 0, which needs
// -Wl,-zmuldefs so the local definition beats the non-weak one. This library does neither.
//
// It was never the frame the check objected to. Disassembling it
// (libnet80211.a(ieee80211_output.o) +0x5c, 434 bytes) shows the path our frames take:
//
//   length 24..1500 ok -> interface ok -> not a protected frame -> type 0 subtype 0xd0
//   (action) accepted -> wifi_get_macaddr(ifidx) -> memcmp(own, &buf[10])
//   -> cnx_node_search(&buf[4]) -> 0x187: beqz.n a5, <log + return ESP_ERR_INVALID_ARG>
//
// a5 is the FOURTH argument: en_sys_seq. Pass false -- as a raw injector naturally does --
// and the check refuses; the reason string sitting at that branch reads, verbatim from the
// blob:
//
//   "en_sys_seq should be true to avoid side-effect to WiFi connection"
//
// which is exactly what esp_wifi.h documents. Passing true is all the check ever wanted,
// and every TX site in this file does. Measured with no override and no -zmuldefs
// anywhere: MIFTX ret=ESP_OK, tx{n=567 ESP_OK}, Mac presence 100.0% over 98s, zero flaps.
//
// Two things it buys beyond the transmission itself. The driver fills the 802.11 sequence
// number when en_sys_seq is true, so hardware retransmits de-duplicate at the receiver
// instead of appearing as ping duplicates. And it removes the coexistence blocker: the
// same rule is why transmission fails outright once any Wi-Fi connection exists.
//
// It is also what makes the library distributable. library.properties CANNOT supply a
// linker flag (measured with a probe library), so a build that needed the override
// could never be a drop-in Arduino library.

// ============================================================================
// Shared state
// ============================================================================

static uint8_t g_awdl_mac[6] = {0};
static struct AwdlPeerTable g_peers;
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;  // core-1-only (proc/loop)

// Our AirDrop identity for the mDNS advertisement (Step 3).
static char g_instance[16] = {0};  // 12-hex service-instance / host label
static char g_pair_sid[48] = {0};  // UUID-shaped session id for the pairing TXT
static uint8_t g_ll[16] = {0};     // our fe80:: link-local (EUI-64 of AWDL mac)
// Custom lwIP-over-AWDL netif (defined further down); declared here so handle_data
// can feed received IPv6 frames up into it.
static esp_netif_t* g_awdl_netif = nullptr;
// esp_timer <-> rx-timestamp offset, measured at RX, used only for TX windowing.
//
// uint32, not int64, and that is load-bearing rather than tidy. Every reader already wrote
// `(uint32_t)(esp_timer_get_time() - g_ts_off32)`, because the rx timestamp is a 32-bit
// free-running counter and the arithmetic downstream is exact modulo 2^32 (AWC_US is 2^20
// and 2^32 is a multiple of it). Truncating at the source is provably the same value --
// (uint32_t)(a-b) == (uint32_t)a - (uint32_t)b -- and it turns "every reader must remember
// to truncate" from a convention into a type. The 71.6-minute wrap is then a compile
// error away rather than a comment away.
//
// It is deliberately NOT part of AwdlWinState: a different task writes it (the frame path,
// on every action frame) at a different rate, and pairing it atomically with the window
// buys nothing -- a stale value differs by the sniffer callback latency, measured at 6 us
// mean and 253 us max, which the PLL already filters.
static volatile uint32_t g_ts_off32 = 0;

// Cached PLL of the selected master (set in awdl_elect_and_publish, read lock-free in the
// proc task) so we can bin each received frame by its AW-cycle phase. Since we
// are parked on ch6, EVERY received frame was sent while its peer was on ch6, so
// the phase histogram reveals the actual ch6 window (which of the 16 slots).
// ...published as ONE object under its own lock. See AwdlWinState in awdl_window.h for
// why: publishing the PLL fields and the mask separately lets a reader land between them
// and pair a fresh phase with the previous election's mask.
//
// g_win_mux is dedicated and is never g_mux. Sharing g_mux would put the window readers --
// which include loop() -- behind the frame path's critical section and vice versa, for two
// pieces of state that have nothing to do with each other.
// How long a lock may be coasted once the elected master goes away. Published and counted
// only -- nothing acts on it yet. Coasting the WINDOW without also coasting the FRAME
// delivers nothing (send_mif refuses an election-less Selection by design, and
// AwdlWinState carries none of the fields the MIF builder needs), so a coast that "works"
// would report success while transmitting zero frames. Measure how far the coasted
// prediction drifts from the first real measurement after it, then decide.
#define AWDL_COAST_MAX_US 20000000u

// The transmit cadence runs in its own task; 0 compiles the identical body into loop().
// See the block above awdl_cadence_pass() for why, and for what may never be called there.
#ifndef AWDL_CADENCE_TASK
#define AWDL_CADENCE_TASK 1
#endif
/* Recency, for the two AwdlStatus fields whose whole point is "right now".
 *
 * MILLISECONDS IN A uint32_t, not microseconds in an int64_t, and that is not an
 * optimisation: these are written by the cadence task at priority 2 and read by the
 * caller's task at priority 1, and a 64-bit read across that boundary on a 32-bit core
 * can tear. Step 9 of this extraction exists because three doubles were being read that
 * way. Unsigned 32-bit subtraction is wrap-exact, so the 49.7-day rollover needs no
 * special case. */
static volatile uint32_t g_last_mif_ms = 0; /* when the radio last ACCEPTED a MIF */
static volatile bool g_mif_seen = false;
static volatile uint32_t g_no_master_since_ms = 0;
static volatile bool g_no_master = false;

static float g_srchist[16];
static uint32_t g_srchist_n;
static void raw_tx_done_cb(const esp_80211_tx_info_t* ti);
static struct AwdlTick g_tick;
static TaskHandle_t g_cadence_task = nullptr;

/* --- cooperative shutdown, for awdl_end() -----------------------------------------
 *
 * The two tasks exit BY THEMSELVES when this is set, and awdl_end() waits for both
 * acknowledgements. It does not vTaskDelete() them, and that is not fastidiousness:
 * proc_task takes g_mux and memcpys into the rings, so a task killed from outside can be
 * killed while holding a spinlock -- which no later begin() can ever release. The
 * cadence task is worse, because it is the thing that transmits: stopping it half way
 * through a frame is a partial write to the radio.
 *
 * volatile, not atomic: one writer (the caller), two readers, and a single bool. */
static volatile bool g_awdl_stop = false;
static volatile bool g_proc_stopped = false;
static volatile bool g_cad_stopped = false;
static int64_t g_cadence_wake_us = 0;

static portMUX_TYPE g_win_mux = portMUX_INITIALIZER_UNLOCKED;
static struct AwdlWinState g_win;  // NOT volatile: only ever touched under g_win_mux
// ...and the election result, published with it and under the same lock.
//
// One WRITER (awdl_elect_and_publish) and any number of READERS (awdl_selection_read).
// Without that split, anything wanting to KNOW the elected master has to re-run the whole
// election to find out -- a display and a status line included -- so four callers per loop
// pass each take g_mux, copy 1,176 bytes and run eight divisions for a value that has not
// changed. And those callers sit at a DIFFERENT priority from the writer, which makes
// Selection's floats cross-priority shared state: a torn s.mean corrupts the status line,
// which is the instrument the cadence is judged by.
static Selection g_sel_pub;

// Copy under the lock; extrapolate outside it. The lock covers a 32-byte structure copy
// and nothing else -- no arithmetic, no floating point, no calls.
static inline void awdl_win_read(struct AwdlWinState* out) {
  taskENTER_CRITICAL(&g_win_mux);
  *out = g_win;
  taskEXIT_CRITICAL(&g_win_mux);
}
// The election result as last published. A pure read: it never elects, never takes g_mux,
// and never allocates a peer row -- so a caller that only wants to DISPLAY the answer
// cannot perturb the thing it is displaying.
// BOTH, under ONE acquisition. Taking two or three separate snapshots of state that is
// published atomically lets one status line carry ch6= from publication N, sel= from N+1
// and MASK used= from N+2, and the cadence task that publishes it is an ordinary
// preemption away from the caller that reads it.
// THE PUBLIC READ. One snapshot, composed from the published window, the published
// election and the cadence's own account of itself -- so a caller can never assemble a
// status out of three different instants. See awdl_status.h for why the derived answers
// live in the header rather than in the caller.

static inline void awdl_pub_read(struct AwdlWinState* w, Selection* sel) {
  taskENTER_CRITICAL(&g_win_mux);
  *w = g_win;
  *sel = g_sel_pub;
  taskEXIT_CRITICAL(&g_win_mux);
}
// GLOBAL window-phase PLL. AWDL synchronises the whole vicinity to ONE availability
// window schedule, so every master's sync frame measures the SAME window phase.
// We feed them ALL into a single PLL instead of a per-master PLL that got reset
// each time the election flapped between equally-synced masters (that reset was
// dropping our lock -- and thus our TX window timing -- whenever nearby Apple
// devices woke up and churned the election). Updated in handle_sync (proc_task),
// read in awdl_elect_and_publish; both core 1, guarded by g_mux.
// The loop itself lives in awdl_peer.h, where it is exercised on the host against a
// verbatim transcription of the version it replaces: 74,000 samples across three streams,
// every field bitwise identical, with 15,641 outliers and 191 re-seeds actually taken
// (tools/test-peer.sh). Written and read on core 1 only, under g_mux.
static struct AwdlPll g_pll;
// Elected master (root of the sync tree we lock to). Declared here (not just
// before awdl_elect_and_publish) so handle_sync can gate the global PLL on mesh membership.
static uint8_t g_sel[6] = {0};
static bool g_have_sel = false;

static const double LOCK_WANDER_US = 1500;  // "locked": smoothed track stable

// ---- sync-stability instrumentation (measurement only, no behaviour change) --
// The three failure modes that are otherwise invisible: the election switching master
// under us, the global PLL hard re-seeding, and frames the gauge discards because their
// sender disagrees with the mesh.
//
// Innovation statistics over ALL sync frames, not split by subtype: there is no systematic
// PSF-vs-MIF offset to separate out, because the gauge in awdl_gauge.h derives the AW grid
// from (rx, aw_seq) alone and never looks at the subtype. Splitting them cuts the noise by
// only 7%, so the ~5.5ms scatter lives INSIDE the PSF population. Two candidate causes,
// and these two instruments separate them:
//   EHIST -- histogram of the PSF innovation. A clean single peak means genuine
//            measurement noise; two peaks ~16384us (one AW) apart means senders
//            disagree on whether aw_seq is the CURRENT or the NEXT AW, i.e. our
//            `aw_seq = raw + 1` is right for some senders and wrong for others.
//   ESRC  -- per-SENDER mean innovation. All senders announce the same sync
//            master, but each has its own clock error; if one sender sits 5ms off
//            the others, the "noise" is really inter-device disagreement and the
//            estimator should weight or exclude per sender, not average blindly.
static struct AwdlSrcTable g_srcs;
// The AW-cycle gauge. Self-contained, dependency-free, and compiled unchanged into
// tools/test-gauge.sh so it can be scored against a recorded trace on a laptop.
static ga_t g_gauge;

// Find (or allocate) this sender's row. Eviction takes the stalest row, so an
// active sender is never displaced. Caller holds g_mux.
/* The table, its eviction rule and the statistics are in awdl_src.h, host-tested there.
   This wrapper binds them to the one instance this firmware has. */
static struct AwdlSrc* src_row(const uint8_t* mac, uint32_t nowms) { return awdl_src_row(&g_srcs, mac, nowms); }
// THE MEASURED ch6 WINDOW, which is the independent check on the chanseq-derived one.
// A raw phase sample, (rx_ts - phy_tx) mod AWC, is our clock offset to THAT SENDER's TSF:
// each sender is individually precise to ~30us, but different senders sit at different TSF
// epochs (three measured at -180ms / +14ms / +275ms relative to each other; EHIST shows
// exactly three spikes, each <2ms wide). Removing that spread is what the mesh gauge in
// awdl_gauge.h is for, and this histogram is how its answer gets checked against the air:
// every frame the PLL source transmits is BY DEFINITION inside its ch6 dwell, so a
// decaying per-slot histogram of exactly that sender's frames IS the window, in our own
// gauge, with no dependence on chanseq indexing or on any TSF-epoch reconstruction.
//
// The cost of having no such check is measured. A mesh whose traffic peaks at AWC slot 0
// in our gauge while chanseq says "ch6 = slot 8" puts every transmission 524ms (AWC/2)
// away from the window, and reachability goes to zero (acc=0 tls=0 disc=0).
static const float SRCHIST_DECAY = 0.99f;   // ~100-frame (~7s) memory
static const float SRCHIST_THRESH = 0.35f;  // ch6 above 35% of (peak - background)

// ============================================================================
// Lock-free SPSC ring: WiFi callback (core0, producer) -> proc task (core1)
// (RawFrame / RING_SLOTS / SNAP_LEN defined in awdl_sync.h)
// ============================================================================
// Reserved only while AWDL is active. Keep callback storage in internal RAM;
// release it only after both workers and the Wi-Fi producer have stopped.
static RawFrame* g_ring = nullptr;
static struct AwdlRing g_ring_cur;  // cursors + drop count (awdl_ring.h)
static_assert(AWDL_RING_POW2(RING_SLOTS), "RING_SLOTS must be a power of two");

// ---- promiscuous RX callback (core 0, WiFi task) — KEEP MINIMAL -------------
static void sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
  int64_t t0 = esp_timer_get_time();
  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
  const uint8_t* pl = pkt->payload;
  const uint16_t len = pkt->rx_ctrl.sig_len;
  if (len < 40) return;

  const uint8_t fc = pl[0];
  // 802.11 type field (bits 2-3): data type covers plain Data (0x08) AND QoS
  // Data (0x88) — real AWDL traffic is usually QoS Data, which an 0x08-only test
  // silently drops.
  bool is_data = ((fc & 0x0c) == 0x08);
  bool is_awdl = false;
  if (is_data) {
    is_awdl = (memcmp(&pl[16], AWDL_BSSID, 6) == 0);
  } else if (fc == 0xd0)
    is_awdl = (memcmp(&pl[24], APPLE_VENDOR, 4) == 0);
  if (!is_awdl) return;

  // Drop rather than wait when the consumer is behind: this is the WiFi task, and
  // blocking here costs the transmit window. awdl_ring counts what was dropped.
  int slot = awdl_ring_reserve(&g_ring_cur);
  if (slot >= 0) {
    RawFrame* f = &g_ring[slot];
    f->rx_ts = pkt->rx_ctrl.timestamp;
    f->now_us = t0;
    f->rssi = pkt->rx_ctrl.rssi;
    uint16_t L = len > SNAP_LEN ? SNAP_LEN : len;
    f->len = L;
    memcpy(f->data, pl, L);
    awdl_ring_commit(&g_ring_cur);
  }
}

// ============================================================================
// Processing (core 1 task). Only this task writes g_peers / counters.
// ============================================================================

/* The table and its eviction rule are in awdl_peer.h, host-tested there. This wrapper
   just binds them to the one instance this firmware has. */
static struct AwdlPeer* master_row(const uint8_t* addr) { return awdl_peer_row(&g_peers, addr); }

// Store EVERY sample; robustness is applied at read time in phase_stats. An insert-time
// outlier gate starves the ring instead.
// Takes the frame's header and its sync TLV ALREADY DECODED by awdl_frame.h, which owns
// the offsets and the length guards and is host-tested -- including the length check on
// the master address that a hand-rolled TLV walk here would not have. See awdl_parse_sync.
static void handle_sync(const struct AwdlActionHdr* h, const struct AwdlSyncParams* sy, uint32_t rx_ts) {
  uint32_t phy_tx = h->phy_tx;
  uint32_t tgt_tx = h->tgt_tx;
  const uint8_t* master = sy->master;
  int64_t remaining_tu = sy->remaining_tu;  // diagnostics only, see below

  // ---- AWC phase from the sender's TSF -------------------------------------
  // The formula the sync TLV invites --
  //     next_aw   = rx_ts + remaining_tu*TU - (phy_tx - tgt_tx)
  //     awc_start = next_aw - ((aw_seq+1) % 64)*AW
  // -- is wrong in two measurable ways, so it is not the one used here:
  //
  //  1. `remaining_aw_length` (val[15..16]) is hard-coded to 0 by every real
  //     sender (Apple and owl alike) -- measured: 0 in essentially every frame.
  //     So that formula silently asserts "the AW boundary is exactly where this
  //     frame arrived". It is not: senders transmit at an arbitrary point inside
  //     their AW (measured `phy_tx % AW` spans 16us..16006us, i.e. the whole AW).
  //     The frame's unknown intra-AW position becomes the error, uniform over one
  //     AW. The innovation histogram shows it exactly: flat from -10ms to +12ms,
  //     not Gaussian, ~1 AW wide, per-sample stdev 5366us.
  //     Corroborating: `aw_seq - floor(phy_tx/AW)` is not constant for a sender
  //     but flips between two adjacent values ~50/50 -- the same +/-1 AW ambiguity.
  //
  //  2. Subtracting (phy_tx - tgt_tx) ADDS noise. rx_ts is simultaneous with
  //     phy_tx, not with tgt_tx: pairing rx_ts with phy_tx gives 31-43us residual
  //     stdev, pairing it with tgt_tx gives 915-1257us with a +5ms tail -- that
  //     tail IS the medium-access delay, which that correction injects.
  //
  // The mesh schedule lives in the sender's TSF, and phy_tx is that TSF at the
  // instant the frame hit the air -- the same instant our rx_ts measures. So the
  // AWC phase is simply the clock offset between us and the sender, mod AWC.
  // Measured per-sample residual: 31-43us, against 5366us for the formula above, and
  // two independent Apple devices agree with each other to 11us jitter / ~1ms
  // constant offset.
  //
  // Wrap safety: AWC_US == 1048576 == 2^20 exactly, and 2^32 is a multiple of
  // 2^20, so this modular subtraction on uint32 counters is exact across both the
  // rx_ts and the phy_tx 32-bit wrap. No 64-bit widening needed, no wrap guard.
  // Raw clock offset: our clock against THIS SENDER's TSF. It becomes an
  // AWC-boundary-anchored phase once the sender's B is known, and B is not solved per
  // sender -- awdl_gauge.h solves the AW grid mesh-wide from (rx, aw_seq) alone and each
  // sender's B is derived from that consensus. Computed below, after the row lookup.
  int64_t offset = (int64_t)((rx_ts - phy_tx) & (uint32_t)(AWC_US - 1));
  (void)tgt_tx;
  (void)remaining_tu;
  const uint16_t aw_seq_raw = sy->aw_seq;

  // 0 = PSF (periodic sync frame), 3 = MIF (master indication frame), -1 = neither.
  // Only PSF reaches the PLL: everything else returns below, before the accumulation,
  // which is also why a per-subtype breakdown of the innovation is structurally
  // unreachable from here.
  const int si = (h->subtype == AWDL_SUBTYPE_PSF) ? 0 : ((h->subtype == AWDL_SUBTYPE_MIF) ? 1 : -1);

  uint32_t nowms = millis();
  taskENTER_CRITICAL(&g_mux);
  struct AwdlPeer* m = master_row(master);
  if (m->last_ms) {
    uint32_t g = nowms - m->last_ms;
    if (g > m->gap_max) m->gap_max = g;
    m->gap_sum += g;
    m->gap_n++;
  }
  m->last_ms = nowms;
  m->last_aw_seq = sy->aw_seq;  // raw announced aw_seq
  m->last_aw_ts = rx_ts;
  m->frames++;
  m->off[m->pos] = offset;
  m->pos = (m->pos + 1) % 16;
  if (m->n < 16) m->n++;

  // --- the mesh gauge, then the window PLL -----------------------------------
  // Everything about the AW-cycle boundary lives in awdl_gauge.h, which is compiled
  // unchanged into a host test (tools/test-gauge.sh) and scored against a recorded trace.
  // That test is the whole point: this is a design whose defects no runtime check can see.
  //
  // WHICH UNKNOWN IT SOLVES FOR, because the obvious one does not work. Estimating each
  // sender's AW-grid origin B from that sender's OWN history and latching the first
  // confident answer scores, on a 5159-frame / 4-sender trace, 20.1% of the phase samples
  // a whole AW or more wrong: two of the four senders have no single valid B at all --
  // their phy_tx takes ~400ms TSF re-base steps, splitting the evidence into clusters
  // 8-19 AW apart -- and such an estimator latches whichever cluster its 16-sample ring
  // happens to hold. In the field that is the gauge 8 AW (131ms, 2 chanseq slots) off,
  // 4125 frames fired into an empty window, and a badge invisible in the AirDrop UI.
  //
  // So the gauge solves for a different unknown. With k = aw_seq mod 64 and
  // h = (rx_ts - k*AW) mod AWC, it satisfies phase in (h - AW, h]: h contains NEITHER
  // phy_tx NOR B, so every frame from every sender constrains the ONE mesh gauge.
  // Verified: the h-interval holds for 71.9-78.1% of frames uniformly across all four
  // senders, the two "unusable" ones included, because phy_tx -- the thing that jumps --
  // cancels out. B is DERIVED from the consensus gauge per sender, never estimated from
  // it. Same trace, same scorer: 99.1% fed, ZERO samples a whole AW wrong, worst error
  // 0.199 AW (5.03x margin to the cliff), gauge locked 1.14s after the first frame.
  const uint8_t* src = h->src;
  struct AwdlSrc* sp = src_row(src, nowms);

  // The TSF-re-base fast path lives inside the gauge now (GA_REBASE_US), where the
  // per-sender clock offset it needs already is. Nothing here to do but keep the row
  // fresh for the ESRC/MSTR lines.
  sp->last_ms = nowms;

  // Only PSFs drive the gauge. There is no PSF-vs-MIF offset to correct for: measured
  // across the two subtypes, the bias is -9us.
  if (si != 0) {
    taskEXIT_CRITICAL(&g_mux);
    return;
  }

  uint32_t gph = ga_on_frame(&g_gauge, src, rx_ts, phy_tx, aw_seq_raw);
  if (gph == GA_NONE) {  // gauge not locked, stale, or this sender
    taskEXIT_CRITICAL(&g_mux);
    return;
  }
  offset = (int64_t)gph;

  double e = 0;
  int pll_r = awdl_pll_feed(&g_pll, offset, rx_ts, &e);
  if (pll_r != AWDL_PLL_SEEDED) {
    awdl_src_add(sp, e);
  }
  taskEXIT_CRITICAL(&g_mux);
}

// Election params v1 (TLV 5): value = flags(1) id(2) distancetop(1) unknown(1)
// top_master_addr(6) top_master_metric(4) self_metric(4) pad(2). We key by the
// ROOT (top_master_addr) and record the root's metric.
static void handle_election(const struct AwdlTlv* t) {
  struct AwdlElection e;
  if (!awdl_parse_election_v1(t, &e)) return;
  taskENTER_CRITICAL(&g_mux);
  struct AwdlPeer* m = master_row(e.master);
  m->metric = e.metric;
  m->have_election = true;
  taskEXIT_CRITICAL(&g_mux);
}

// Election params v2 (TLV 24): value = master_addr(6) sync_addr(6) master_counter(4)
// distance_to_master(4) master_metric(4) self_metric(4) unknown(4) reserved(4)
// self_counter(4). Modern iPhones use this; the master_counter is the freshness
// counter iOS tracks -- we must echo the ROOT's real counter so we look in-mesh
// (not frozen at 0). Keyed by master_addr (root).
static void handle_election_v2(const struct AwdlTlv* t) {
  struct AwdlElection e;
  if (!awdl_parse_election_v2(t, &e)) return;
  taskENTER_CRITICAL(&g_mux);
  struct AwdlPeer* m = master_row(e.master);
  m->master_counter = e.counter;
  if (e.metric) m->metric = e.metric;  // prefer v2's master_metric when present
  m->have_election = true;
  taskEXIT_CRITICAL(&g_mux);
}

static void handle_chanseq(const uint8_t* src, const struct AwdlTlv* t) {
  struct AwdlChanSeq cs;
  if (!awdl_parse_chanseq(t, &cs)) return;
  taskENTER_CRITICAL(&g_mux);
  struct AwdlPeer* m = master_row(src);
  memcpy(m->chseq, cs.slot, 16);
  m->have_chseq = true;
  taskEXIT_CRITICAL(&g_mux);
}

// ICMPv6 checksum over the IPv6 pseudo-header (next-header = 58).
static uint16_t icmp6_checksum(const uint8_t* src, const uint8_t* dst, const uint8_t* d, int len) {
  uint32_t s = 0;
  for (int i = 0; i < 16; i += 2) s += (src[i] << 8) | src[i + 1];
  for (int i = 0; i < 16; i += 2) s += (dst[i] << 8) | dst[i + 1];
  s += (uint32_t)len;
  s += 58;
  for (int i = 0; i + 1 < len; i += 2) s += (d[i] << 8) | d[i + 1];
  if (len & 1) s += d[len - 1] << 8;
  while (s >> 16) s = (s & 0xffff) + (s >> 16);
  uint16_t c = ~s;
  return c ? c : 0xffff;
}

// Seed lwIP's ND6 neighbor cache for an AWDL peer WITHOUT real NDP. We synthesize
// a Neighbor Solicitation *from* the peer (with a Source-Link-Layer-Address option
// = peer MAC, target = our fe80) and feed it up the stack; RFC 4861 makes lwIP
// create/update the peer's neighbor entry (STALE) from the SLLAO. AWDL never does
// NDP (fe80 <-> MAC is deterministic EUI-64), so without this our replies point at
// an unresolvable neighbor and the ND6 retransmit/queue churn reboots us on the
// first inbound unicast. This is the "add discovered AWDL peer to the neighbor
// table" step that Apple's stack and OWL perform.
static void seed_neighbor(const uint8_t* peer_mac, const uint8_t* peer_ll) {
  if (!g_awdl_netif) return;
  uint8_t* f = (uint8_t*)malloc(14 + 40 + 32);  // esp_netif refs+frees via eb
  if (!f) return;
  uint8_t* p = f;
  memcpy(p, g_awdl_mac, 6);
  p += 6;  // eth dst = us
  memcpy(p, peer_mac, 6);
  p += 6;  // eth src = peer
  *p++ = 0x86;
  *p++ = 0xdd;
  *p++ = 0x60;
  *p++ = 0;
  *p++ = 0;
  *p++ = 0;  // IPv6
  *p++ = 0;
  *p++ = 32;  // payload length = 32
  *p++ = 58;
  *p++ = 255;  // next=ICMPv6, hop limit 255
  memcpy(p, peer_ll, 16);
  p += 16;  // src = peer fe80
  memcpy(p, g_ll, 16);
  p += 16;  // dst = our fe80
  uint8_t* icmp = p;
  *p++ = 135;
  *p++ = 0;  // NS, code 0
  *p++ = 0;
  *p++ = 0;  // checksum (filled below)
  *p++ = 0;
  *p++ = 0;
  *p++ = 0;
  *p++ = 0;  // reserved
  memcpy(p, g_ll, 16);
  p += 16;  // target = our fe80
  *p++ = 1;
  *p++ = 1;  // opt: SLLAO, length 1 (8 bytes)
  memcpy(p, peer_mac, 6);
  p += 6;  // peer MAC
  uint16_t ck = icmp6_checksum(peer_ll, g_ll, icmp, 32);
  icmp[2] = ck >> 8;
  icmp[3] = ck & 0xff;
  esp_netif_receive(g_awdl_netif, f, (int)(p - f), f);  // eb=f -> rx_free frees it
}

// Keep each peer's ND6 entry FRESH (not just seed once). lwIP's neighbor cache is
// tiny (LWIP_ND6_NUM_NEIGHBORS ~3) so with several AWDL peers around, a peer we
// seeded gets evicted; once gone, lwIP falls back to real NDP (NS) to reply, which
// AWDL never answers -> our SYN-ACK/replies stall (observed on Device 2 as our NS
// storm + no SYN-ACK). So re-seed a peer whenever we hear from it and >500ms have
// passed, and evict round-robin when the small cache is full.
static uint8_t g_seeded[8][6];
static uint32_t g_seed_ms[8];
static int g_nseeded = 0;
//
// LOCKED, and that became necessary rather than tidy when awdl_seed_peer() gave this a
// SECOND caller. With only the frame task calling it, `g_nseeded++` was safe by having
// one writer; with loopTask seeding a send target too, two tasks can both read
// g_nseeded == 7, both increment, and the second one writes g_seeded[8] -- one row past
// a 8-row array. g_mux is the right lock: it is documented core-1-only, and proc_task
// and loopTask are both on core 1.
//
// seed_neighbor() stays OUTSIDE the critical section. It mallocs and calls
// esp_netif_receive(); doing either inside portENTER_CRITICAL is its own bug.
static void seed_refresh(const uint8_t* peer_mac, const uint8_t* peer_ll) {
  uint32_t now = millis();
  bool doit = false;
  portENTER_CRITICAL(&g_mux);
  int i = 0;
  for (; i < g_nseeded; i++)
    if (memcmp(g_seeded[i], peer_mac, 6) == 0) break;
  if (i < g_nseeded) {
    if ((uint32_t)(now - g_seed_ms[i]) >= 500) {
      g_seed_ms[i] = now;
      doit = true;
    }
  } else {
    int idx = (g_nseeded < 8) ? g_nseeded++ : (int)(now % 8);  // evict pseudo-randomly when full
    memcpy(g_seeded[idx], peer_mac, 6);
    g_seed_ms[idx] = now;
    doit = true;
  }
  portEXIT_CRITICAL(&g_mux);
  if (doit) seed_neighbor(peer_mac, peer_ll);
}

// --- reaching a peer we have only HEARD --------------------------------------------
//
// The receive path seeds a peer at the moment it addresses US (the for_us branch in
// handle_data). That is everything a receiver needs and nothing a sender can use: an
// outbound target has never sent us a unicast frame, so ND6 holds nothing for it, lwIP
// falls back to real NDP, and AWDL never answers an NS. The connect stalls behind an
// unresolvable neighbour -- the exact failure seed_neighbor() was written to prevent,
// arrived at from the other direction.
void awdl_ll_of_mac(const uint8_t mac[6], uint8_t out_ll[16]) {
  memset(out_ll, 0, 16);
  out_ll[0] = 0xfe;
  out_ll[1] = 0x80;
  out_ll[8] = mac[0] ^ 0x02;
  out_ll[9] = mac[1];
  out_ll[10] = mac[2];
  out_ll[11] = 0xff;
  out_ll[12] = 0xfe;
  out_ll[13] = mac[3];
  out_ll[14] = mac[4];
  out_ll[15] = mac[5];
}

void awdl_seed_peer(const uint8_t mac[6]) {
  uint8_t ll[16];
  awdl_ll_of_mac(mac, ll);
  seed_refresh(mac, ll);
}

int awdl_netif_index(void) { return g_awdl_netif ? esp_netif_get_netif_impl_index(g_awdl_netif) : 0; }

static void handle_data(const uint8_t* pl, uint16_t len) {
  int p = -1;
  for (int i = 30; i <= 46 && i + 1 < len; i++)
    if (pl[i] == 0x86 && pl[i + 1] == 0xdd) {
      p = i + 2;
      break;
    }
  if (p < 0 || p + 48 > len) return;
  const uint8_t* ip = &pl[p];
  if ((ip[0] >> 4) != 6) return;
  const uint8_t* src = &ip[8];
  // Feed frames addressed to US (or our NDP multicast groups) up into lwIP so the
  // real stack does NDP (auto-NA), TCP, TLS, HTTP. Synthesize the 14-byte
  // Ethernet header lwIP's ETH netstack expects. mDNS multicast is answered by the
  // layer above, through the RX tap below; we still forward it here so any unicast
  // follow-up resolves. esp_netif_receive copies synchronously (eb=NULL).
  // COMPUTED HERE, outside the netif-push test below, and that is load-bearing rather
  // than tidy. The RX tap needs it, and an mDNS query to ff02::fb satisfies none of
  // for_us, sol_node or all_nodes -- so computed inside that branch it would be invisible
  // to exactly the traffic the tap exists for.
  const int iplen = (int)len - (int)(ip - pl);
  // R = received by the ESP frame task, before netif delivery and the AirDrop tap.
  {
    bool for_us = (memcmp(&pl[4], g_awdl_mac, 6) == 0);
    bool sol_node = (pl[4] == 0x33 && pl[5] == 0x33 && pl[6] == 0xff && pl[7] == g_awdl_mac[3] &&
                     pl[8] == g_awdl_mac[4] && pl[9] == g_awdl_mac[5]);
    bool all_nodes =
        (pl[4] == 0x33 && pl[5] == 0x33 && pl[6] == 0 && pl[7] == 0 && pl[8] == 0 && pl[9] == 0x01);  // ff02::1
    // Feed EVERY for_us/multicast frame to lwIP -- do NOT drop 802.11 L2-retransmit
    // duplicates (Retry bit). Dropping them looks free and is not: if we miss a
    // frame's ORIGINAL transmission (busy TXing) and hear only its L2 retransmit,
    // dropping that means lwIP never sees the data at all -- e.g. a ClientHello
    // arrives only as a retransmit, gets dropped, and select() never marks the socket
    // readable (observed: rdbl=0 with ClientHellos present). lwIP's TCP de-dups by
    // sequence number correctly, so passing duplicates is safe.
    if (for_us) {
      // Seed ND6 with this peer (deterministic fe80<->MAC) BEFORE lwIP replies,
      // so it never enters unresolvable-neighbor NDP churn.
      if (src[0] == 0xfe && src[1] == 0x80) seed_refresh(&pl[10], src);
    }
    if (g_awdl_netif && (for_us || sol_node || all_nodes)) {
      // 1500 on RECEIVE, while transmit is capped at AWDL_NETIF_MTU (1280). The
      // asymmetry is deliberate, not an oversight to tidy up: we advertise no link MTU to
      // the peer -- AWDL carries no router advertisement -- lwIP's ip6_input applies no
      // MTU test of its own, and macOS is under no obligation to send us anything smaller
      // than a frame will hold. What is refused here is what lwIP never sees.
      if (iplen > 0 && iplen <= 1500) {
        // esp_netif REFERENCES this buffer (PBUF_REF) and frees it later via
        // awdl_netif_rx_free(eb) -- so give it its own heap buffer, never a reused
        // static (which lwIP could hold past our next frame).
        uint8_t* rxb = (uint8_t*)malloc(14 + iplen);
        if (rxb) {
          memcpy(rxb, &pl[4], 6);       // dst (us / multicast)
          memcpy(rxb + 6, &pl[10], 6);  // src = peer
          rxb[12] = 0x86;
          rxb[13] = 0xdd;
          memcpy(rxb + 14, ip, iplen);
          (void)esp_netif_receive(g_awdl_netif, rxb, 14 + iplen, rxb);
        }
      }
    }
  }

  // THE RX TAP. Beside the lwIP push and after it, non-consuming: the AirDrop layer reads
  // the same frame and may answer it from here, which is the whole point -- an mDNS query
  // is answered at the instant it is heard, the only moment the querier is provably on
  // channel 6. Deferring to the netif queue can cost a full 1,048,576 us cycle.
  //
  // It runs ON THE FRAME TASK, so invariant (a) is the tap's contract too: no Serial, no
  // lwIP, no blocking. Immediate TX is guarded by g_in_tap.
  if (g_rx_tap && iplen > 0) {
    g_in_tap = true;
    g_rx_tap(g_rx_tap_ctx, &pl[10], ip, (size_t)iplen);
    g_in_tap = false;
  }
}

static void process_frame(const RawFrame* f) {
  const uint8_t* pl = f->data;
  const uint16_t len = f->len;
  const uint8_t fc = pl[0];

  // Bin this frame by its AW-cycle phase (we only hear ch6, so the histogram is
  // the ch6 window). Split out device1's own TX to check window alignment.
  struct AwdlWinState _w;
  awdl_win_read(&_w);
  if (_w.have_lock) {
    struct AwdlPll snap;
    awdl_pll_init(&snap);
    snap.phase = _w.phase;
    snap.freq = _w.freq;
    snap.tlast = _w.tlast;
    int64_t pos = awdl_pll_cycle_phase(&snap, f->rx_ts);
    int bin = (int)(pos / (AWC_US / 16));
    if (bin >= 0 && bin < 16) {
      // Decaying histogram of EVERY heard frame, binned in the common gauge. Each
      // frame was emitted while its sender was dwelling on ch6, and per-sender
      // epoch correction now puts all senders in one gauge, so the hot slots of
      // this histogram ARE the mesh's ch6 window -- measured, in our own clock,
      // with no dependence on chanseq indexing or on any TSF-epoch reconstruction.
      // Skip our own transmissions: they would make the mask self-confirming.
      if (memcmp(&pl[10], g_awdl_mac, 6) != 0) {
        taskENTER_CRITICAL(&g_mux);
        for (int k = 0; k < 16; k++) g_srchist[k] *= SRCHIST_DECAY;
        g_srchist[bin] += (1.0f - SRCHIST_DECAY);
        g_srchist_n++;
        taskEXIT_CRITICAL(&g_mux);
      }
    }
  }

  if ((fc & 0x0c) == 0x08) {  // data frame (plain or QoS)
    handle_data(pl, len);
  } else {  // action frame
    g_ts_off32 = (uint32_t)(f->now_us - (int64_t)f->rx_ts);
    // Validate the AWDL action header once, up front. sniffer_cb checks only the vendor
    // OUI, which decides the frame is ours and says nothing about the header being
    // complete: a TLV walk from a hard-coded pl[40] would trust a truncated frame.
    struct AwdlActionHdr h;
    if (!awdl_parse_action(pl, len, &h)) return;
    const uint8_t* src = h.src;
    const uint8_t* cur = h.tlv;
    struct AwdlTlv t;
    while (awdl_tlv_next(&cur, h.tlv_end, &t)) {
      switch (t.tag) {
        case TLV_SYNC_PARAMS: {
          struct AwdlSyncParams sy;
          if (!awdl_parse_sync(&t, &sy)) break;
          handle_sync(&h, &sy, f->rx_ts);
        } break;
        case TLV_ELECTION_PARAMS:
          handle_election(&t);
          break;
        case TLV_ELECTION_V2:
          handle_election_v2(&t);
          break;
        case TLV_CHAN_SEQ:
          handle_chanseq(src, &t);
          break;
        default:
          break;
      }
    }
  }
}

static void proc_task(void*) {
  for (;;) {
    /* Checked at the TOP of the loop, outside the drain, so the task never leaves with a
       ring slot half-processed or g_mux held. */
    if (g_awdl_stop) {
      g_proc_stopped = true;
      vTaskDeleteWithCaps(nullptr);
    }
    bool did = false;
    for (int slot; (slot = awdl_ring_peek(&g_ring_cur)) >= 0;) {
      process_frame(&g_ring[slot]);
      awdl_ring_release(&g_ring_cur);
      did = true;
    }
    if (!did) vTaskDelay(1);  // ~1 tick idle; bursts drain immediately
  }
}

// ============================================================================
// Stats / election (read side, core 1 loop). Snapshot under a SHORT lock, then
// compute outside it so the critical section stays tiny.
// ============================================================================

// True if this MAC has proven to be an unusable timing source (chronic TSF
// re-baser). Such a peer must not be elected either: the one measured doing it was
// also winning on metric, so the election kept flapping to it and dragging both the
// advertised master and the chanseq-derived TX mask with it (842 switches in 4min).
static bool src_untrusted(const uint8_t* mac) {
  // This gates the ELECTION only -- i.e. which master address we advertise and whose
  // channel sequence we take the TX mask from. It deliberately does NOT gate the
  // gauge: the gauge does not depend on any sender's TSF being sane, and
  // replayed alone the trace's two chronic re-basers feed 410 and 1486 phase samples
  // with zero errors. Whether a re-baser's *chanseq* is equally trustworthy is not
  // something I have measured, so this exclusion stays until it is.
  return ga_rebase_of(&g_gauge, mac) >= 4;
}

static uint8_t s_win_state = AWDL_WS_VOID;  // publisher-private: never read g_win unlocked

// THE SINGLE WRITER of the transmit gate and of the published Selection.
//
// Named for what it does rather than what it returns, because this must have exactly one
// calling task. Anything that merely wants to know the answer calls awdl_selection_read()
// instead.
static Selection awdl_elect_and_publish() {
  uint32_t now = millis();
  struct AwdlWinState nw;
  awdl_win_read(&nw);  // start from what is live; each branch
                       // then overwrites what it owns
  // snapshot rings under a short lock
  int64_t off[AWDL_PEERS][16];
  int n[AWDL_PEERS];
  uint32_t metric[AWDL_PEERS], last[AWDL_PEERS], gapavg[AWDL_PEERS];
  uint8_t addr[AWDL_PEERS][6];
  bool used[AWDL_PEERS];
  taskENTER_CRITICAL(&g_mux);
  for (int i = 0; i < AWDL_PEERS; i++) {
    used[i] = g_peers.row[i].used;
    n[i] = g_peers.row[i].n;
    metric[i] = g_peers.row[i].metric;
    last[i] = g_peers.row[i].last_ms;
    gapavg[i] = g_peers.row[i].gap_n ? g_peers.row[i].gap_sum / g_peers.row[i].gap_n : 0xffffffff;
    memcpy(addr[i], g_peers.row[i].addr, 6);
    memcpy(off[i], g_peers.row[i].off, sizeof(off[i]));
  }
  taskEXIT_CRITICAL(&g_mux);

  // Elect the mesh master by CONSENSUS: the master address announced most often
  // (lowest mean inter-announcement gap), with metric only as a tie-break.
  //
  // Metric-first is wrong for what this choice is actually for. The phase gauge does not
  // depend on it, so the elected master's only jobs are (a) the
  // master_addr + master_counter we echo in our own MIF, which is how we look like
  // a credible in-mesh peer, and (b) the chanseq we take the TX window from. Both
  // want the address the mesh AGREES on. Metric-first instead tracked whichever
  // peer momentarily claimed the biggest number -- measured flapping 842 times in
  // four minutes between the Mac (a steady 510) and a peer whose metric swung
  // 515..540 while re-basing its TSF twice a second. The badge left the Mac's AWDL
  // peer table exactly when we started advertising that peer as our master.
  // Gap is a stable, physically meaningful quantity: the master everyone announces
  // is the master everyone is synced to.
  Selection s;
  s.idx = -1;
  s.mean = 0;
  s.stdev = 0;
  s.metric = 0;
  s.count = 0;
  s.wander = 0;
  s.resid_rms = 0;
  s.drift_ppm = 0;
  s.pll_n = 0;
  s.cur_aw_seq = 0;
  s.master_counter = 0;
  s.have_election = false;
  memset(s.addr, 0, 6);
  // The choice itself lives in awdl_elect.h as a pure function over a candidate
  // array, so the rules that twice took this badge off the air can be exercised on
  // the host (tools/test-elect.sh) instead of only on the device. Everything the
  // firmware needs beyond the choice -- phase statistics, the global PLL, the ch6
  // mask -- stays here, below.
  struct ElectCand cnd[AWDL_PEERS];
  for (int i = 0; i < AWDL_PEERS; i++) {
    cnd[i].used = used[i];
    memcpy(cnd[i].addr, addr[i], 6);
    cnd[i].n = (uint32_t)n[i];
    cnd[i].metric = metric[i];
    cnd[i].last_ms = last[i];
    cnd[i].gap_avg = gapavg[i];
    float mean_i, std_i;
    phase_stats_raw(off[i], n[i], &mean_i, &std_i);
    cnd[i].stdev = std_i;
    cnd[i].untrusted = src_untrusted(addr[i]);
  }
  struct ElectResult er = elect_choose(cnd, AWDL_PEERS, now, g_awdl_mac, g_sel, g_have_sel);
  int chosen = er.chosen;
  s.count = er.count;
  if (chosen < 0) {
    // No candidate. The GATE is untouched -- it keeps whatever window it last had, exactly
    // as it did when these were six separate globals -- but the state decays so the time
    // spent without an election is counted rather than inferred.
    uint32_t rx_now = (uint32_t)(esp_timer_get_time() - g_ts_off32);
    s_win_state = awdl_winstate_next(s_win_state, false, nw.ch6_mask, rx_now, nw.lock_rx, AWDL_COAST_MAX_US);
    nw.state = s_win_state;
  }
  if (chosen >= 0) {
    memcpy(g_sel, addr[chosen], 6);
    g_have_sel = true;
    float mean, stdev;
    phase_stats_raw(off[chosen], n[chosen], &mean, &stdev);
    s.idx = chosen;
    s.stdev = stdev;
    s.metric = metric[chosen];  // stdev = raw, info only
    memcpy(s.addr, addr[chosen], 6);
    // Window phase from the GLOBAL PLL (continuous across master switches); the
    // aw_seq base still comes from the chosen master (for our advertised counter).
    double phase, freq, wander, resid;
    uint32_t tlast, pn, aw_ts;
    uint16_t aw_base;
    uint32_t mcnt;
    bool hel;
    taskENTER_CRITICAL(&g_mux);
    phase = g_pll.phase;
    freq = g_pll.freq;
    tlast = g_pll.tlast;
    wander = g_pll.wander;
    resid = g_pll.resid;
    pn = g_pll.n;
    aw_base = g_peers.row[chosen].last_aw_seq;
    aw_ts = g_peers.row[chosen].last_aw_ts;
    mcnt = g_peers.row[chosen].master_counter;
    hel = g_peers.row[chosen].have_election;
    taskEXIT_CRITICAL(&g_mux);
    s.master_counter = mcnt;
    s.have_election = hel;
    uint32_t rx_now = (uint32_t)(esp_timer_get_time() - g_ts_off32);
    double predicted = phase + freq * (double)(uint32_t)(rx_now - tlast);
    predicted = fmod(predicted, (double)AWC_US);
    if (predicted < 0) predicted += AWC_US;
    s.mean = (float)predicted;
    s.cur_aw_seq = (uint16_t)(aw_base + (uint32_t)(rx_now - aw_ts) / (uint32_t)AW_US);
    // TX gating: prefer the MEASURED window; fall back to chanseq until the
    // histogram is confident (and keep both for the MASK cross-check line).
    int mpk = -1;
    uint16_t mask_meas = ch6_mask_measured(&mpk);
    uint16_t mask_chanseq = ch6_mask_of(addr[chosen]);
    // The channel sequence is the authority: with a B-anchored gauge its slot
    // numbering is meaningful again, it is stable, and every master observed puts
    // ch6 at the same slot. The measured mask is only a cross-check -- when the two
    // agree, the gauge is provably aligned; when they disagree, the gauge has
    // drifted (which is exactly how the walk above was caught). Never gate on the
    // measurement alone: a drifted gauge then aims our transmissions at nothing.
    // NO rotation of the mask. Rotating it papers over a gauge that is a whole number of
    // AWs out, and it corrects only the MASK -- s.mean, cur_aw_seq, the live sync-field
    // overwrite in build_mif and the receive histogram stay misaligned. With the
    // gauge derived from the mesh-wide h-constraint that error mode does not arise. The
    // MASK line keeps printing meas vs chanseq: their agreement is the human-visible check
    // that the gauge is aligned, and the gauge's own 32-frame health register is the
    // machine-visible one.
    // ONE publish, at the end of the function and AFTER the mask is decided. Publishing
    // the PLL fields earlier lets a reader land between the two and pair a fresh phase
    // with the previous election's mask, aiming the window at a slot the new master does
    // not listen on.
    // NO redeclaration of nw here. A declaration inside this block shadows the
    // function-scope object: all eight assignments below land in a copy that dies at the
    // closing brace, while the publish at the end of the function writes the outer one --
    // which nothing has then set have_lock on. g_win is BSS-zeroed, so the transmit gate
    // stays shut for the entire life of the boot, and it builds clean and passes all
    // thirteen host suites while the badge transmits nothing at all.
    nw.phase = phase;
    nw.freq = freq;
    nw.tlast = tlast;
    nw.have_lock = true;
    nw.meas_mask = mask_meas;
    nw.chanseq_mask = mask_chanseq;  // published together
    if (mask_chanseq) {
      nw.ch6_mask = mask_chanseq;
      nw.ch6_slot = (int8_t)ch6_slot_of(addr[chosen]);
    } else {
      nw.ch6_mask = mask_meas;
      nw.ch6_slot = (int8_t)mpk;
    }
    // lock_rx means "when we last had a window we could transmit into" -- which is the
    // thing a coast would be coasting -- so it advances only when there IS a mask.
    if (nw.ch6_mask) nw.lock_rx = rx_now;
    // Published and COUNTED, never consulted by the gate. Passing rx_now as BOTH now_rx
    // and lock_rx was the other half of this bug: it made the coast test a tautology that
    // could never expire.
    s_win_state = awdl_winstate_next(s_win_state, nw.ch6_mask != 0, nw.ch6_mask, rx_now, nw.lock_rx, AWDL_COAST_MAX_US);
    nw.state = s_win_state;
    s.wander = (float)wander;
    s.resid_rms = (float)sqrt(resid);
    s.drift_ppm = (float)(freq * 1e6);
    s.pll_n = pn;
  }
  // ONE publish, at the end, with BOTH objects complete. Publishing mid-function sends
  // the Selection out four fields short, because wander / resid_rms / drift_ppm / pll_n
  // are assigned after the window is decided -- and the RETURNED value would still be
  // right while the PUBLISHED one was wrong, which is exactly the kind of difference that
  // survives a soak and surfaces as a display disagreeing with itself.
  taskENTER_CRITICAL(&g_win_mux);
  g_win = nw;
  g_sel_pub = s;
  taskEXIT_CRITICAL(&g_win_mux);
  return s;
}

// The ch6 window as MEASURED from the PLL source's own transmissions (see the
// g_srchist comment). Returns 0 while not yet confident, so the caller can fall
// back to the chanseq-derived mask. Also reports the peak slot via *peak.
static uint16_t ch6_mask_measured(int* peak) {
  float h[16];
  uint32_t n;
  taskENTER_CRITICAL(&g_mux);
  memcpy(h, g_srchist, sizeof(h));
  n = g_srchist_n;
  taskEXIT_CRITICAL(&g_mux);
  if (peak) *peak = -1;
  if (n < 64) return 0;  // not enough evidence yet
  float mx = 0;
  int pk = -1;
  for (int k = 0; k < 16; k++)
    if (h[k] > mx) {
      mx = h[k];
      pk = k;
    }
  if (mx <= 0) return 0;
  // There is a floor of off-window traffic on ch6 (other meshes, retries, devices
  // not following this schedule) at roughly a quarter of the peak, so a plain
  // fraction-of-peak threshold selects almost every slot. Subtract the median
  // first: that is the background level, and what rises above it is the window.
  float srt[16];
  memcpy(srt, h, sizeof(srt));
  for (int i = 1; i < 16; i++) {
    float k2 = srt[i];
    int j = i - 1;
    while (j >= 0 && srt[j] > k2) {
      srt[j + 1] = srt[j];
      j--;
    }
    srt[j + 1] = k2;
  }
  float med = 0.5f * (srt[7] + srt[8]);
  if (mx - med < 4.0f * (1.0f - SRCHIST_DECAY)) return 0;  // no real peak yet
  uint16_t m = 0;
  for (int k = 0; k < 16; k++)
    if (h[k] - med > SRCHIST_THRESH * (mx - med)) m |= (uint16_t)(1u << k);
  if (peak) *peak = pk;
  return m;
}

static int ch6_slot_of(const uint8_t* addr) {
  int idx = -1;
  taskENTER_CRITICAL(&g_mux);
  const struct AwdlPeer* m = awdl_peer_find(&g_peers, addr);
  if (m && m->have_chseq) idx = awdl_chanseq_first_of(m->chseq, PROBE_CHANNEL);
  taskEXIT_CRITICAL(&g_mux);
  return idx;
}

// Bitmask of EVERY ch6 slot in `addr`'s channel sequence (bit k = AWC slot k).
// The mesh densifies ch6 under traffic (1/16 -> 5/16 observed during a transfer);
// gating TX to only the first ch6 slot wasted that extra airtime, so we track all
// of them and let in_ch6_window_now() transmit across the whole ch6 run.
static uint16_t ch6_mask_of(const uint8_t* addr) {
  uint16_t mask = 0;
  taskENTER_CRITICAL(&g_mux);
  const struct AwdlPeer* m = awdl_peer_find(&g_peers, addr);
  if (m && m->have_chseq) mask = awdl_chanseq_mask_of(m->chseq, PROBE_CHANNEL);
  taskEXIT_CRITICAL(&g_mux);
  return mask;
}
// Adapter onto awdl_frame.h, which owns the frame itself. Everything the builder used
// to read from a global -- our instance label, the clock, the elected master's metric
// and counter, the window phase -- is passed in here, so the bytes can be exercised on
// a host against golden vectors (tools/test-frame.sh) instead of only on the device.
//
// ONE clock read, at the top, for phy_tx and for the live remaining_aw_length both.
// Reading it a second time at the bottom would put the whole frame build between the two
// samples; remaining_aw_length is quantised to TU_US = 1024us, so the difference cannot
// change the emitted value except exactly on a boundary -- and a capture showed
// that most real senders leave this field at 0 anyway and that our own phase estimator
// does not read it.
static size_t build_mif(uint8_t* b, const uint8_t* src, const uint8_t* master, const Selection& s) {
  const int64_t t = esp_timer_get_time();
  struct AwdlMifParams m;
  memset(&m, 0, sizeof m);
  m.src = src;
  m.master = master;
  m.instance = g_instance;
  m.tsf_us = (uint32_t)t;
  m.metric = s.metric;
  m.master_counter = s.master_counter;
  m.have_window = (s.idx >= 0);
  m.rx_now_us = (uint32_t)(t - g_ts_off32);
  m.phase_us = (uint32_t)s.mean;  // s.mean is wrapped into [0, AWC_US) and
                                  // AWC_US < 2^24, so float -> uint32 is exact
  m.cur_aw_seq = s.cur_aw_seq;
  return awdl_build_mif(b, AWDL_MIF_MAX, &m);
}

// Send a full MIF (subtype 3). This is the peering-critical frame: it is what
// makes the phone set sent_mif and read our version/devclass + data-path-state.
static void send_mif(const Selection& s) {
  static uint8_t buf[AWDL_MIF_MAX];  // the builder refuses anything smaller
  // Precondition, enforced rather than assumed: there IS an elected master. The only call
  // site, in awdl_cadence_pass(), is gated on s.idx >= 0, and there is deliberately no
  // fallback here that advertises g_awdl_mac when s.idx < 0. Such a fallback is
  // unreachable today and a landmine tomorrow: anyone adding a ride-through that keeps
  // transmitting through an election gap would call this with s.idx < 0 and put a MIF on
  // the air naming OURSELVES as the mesh root. We advertise distancetop=1 with a weak
  // self_metric precisely because claiming root makes iOS read us as out-of-mesh and
  // refuse to peer, and a peer that echoed that back would seed the self-election
  // awdl_elect_and_publish() guards against. Refuse if the precondition is violated.
  if (s.idx < 0) {
    return;
  }
  uint8_t master[6];
  memcpy(master, s.addr, 6);
  size_t n = build_mif(buf, g_awdl_mac, master, s);
  esp_err_t mtx = esp_wifi_80211_tx(WIFI_IF_AP, buf, n, true);
  if (mtx == ESP_OK) {
    g_last_mif_ms = (uint32_t)(esp_timer_get_time() / 1000);
    g_mif_seen = true;
  }
}

/* --- surface B/G: inject one complete IPv6 packet ---------------------------------
 *
 * The port owns the 802.11 + LLC + awdl_data wrap; the caller owns everything from the
 * IPv6 header inwards. The wrap is fixed: build_awdl_data_at() with seq 0, whose
 * awdl_data header is the eight bytes 03 04 00 00 00 00 86 dd.
 *
 * ENFORCED, not documented-and-hoped. This transmits IMMEDIATELY, outside the window
 * gate, which is legal only at a moment the port has already established is on-channel:
 * inside an RX tap (the querier is transmitting, so it is listening) or inside the
 * announce callback (the cadence just decided the window is open). Called from anywhere
 * else it refuses.
 *
 * IT IS NOT CHEAP, and the number is part of the contract: measured 3,400-7,379 us of
 * frame-path time per call against a 319 us baseline -- 20-45 % of one AW. Splitting it
 * showed the 1,027-byte BUILD is a flat 223-261 us and the synchronous esp_wifi_80211_tx
 * is 298-7,073 us, so the cost cannot be removed, only moved off the task. */
int awdl_tx_ip6_now(const uint8_t dst[6], uint8_t* buf, size_t len) {
  if (!g_in_tap) {
    return -1;
  }
  /* 1400 of IPv6, and deliberately NOT AWDL_NETIF_MTU. This path bypasses the netif
   * entirely -- it is how a reply reaches the air from inside an RX tap -- so the netif's
   * 1280-byte cap does not constrain it. What does is esp_wifi_80211_tx, which refuses a
   * frame over 1500 bytes: with AWDL_TX_HEADROOM on top of the packet, 1460 is the true
   * maximum, and 1400 keeps the same deliberate margin the netif MTU takes, expressed in
   * the units this caller works in. */
  if (!buf || len == 0 || len > 1400) return -1;
  /* Built IN PLACE, in the caller's buffer, because the port has no business owning a
   * second 1,600-byte copy of a frame the caller already holds. Internal RAM is the
   * binding resource on this chip -- mbedTLS wants ~31.5 KB of it in one piece and
   * allocates MALLOC_CAP_INTERNAL only -- and one frame-sized copy buffer here is measured
   * to be enough to push ssl_setup into -0x7f00 (ALLOC_FAILED). Every static byte the
   * library takes is a byte TLS does not get. */
  build_awdl_data_at(buf, dst, 0x86dd, (int)len, 0);
  esp_err_t e = esp_wifi_80211_tx(WIFI_IF_AP, buf, (int)(AWDL_TX_HEADROOM + len), true);
  return e == ESP_OK ? 0 : -1;
}

/* --- the RX tap ------------------------------------------------------------------- */
int awdl_rx_tap_set(awdl_rx_tap_fn fn, void* ctx) {
  g_rx_tap = fn;
  g_rx_tap_ctx = ctx;
  return 0;
}

/* --- the announce hook -------------------------------------------------------------
 *
 * The cadence decides WHEN to announce -- it is the only thing that knows where the
 * window is -- and the upper layer decides WHAT an announcement is. Registration is what
 * keeps that from being a call out of src/awdl into src/airdrop. */
static void (*g_announce_cb)(void*) = nullptr;
static void* g_announce_ctx = nullptr;

int awdl_set_announce_cb(void (*fn)(void* ctx), void* ctx) {
  g_announce_cb = fn;
  g_announce_ctx = ctx;
  return 0;
}

/* --- diagnostics the tap is allowed to use ------------------------------------------ */
// ============================================================================
// Custom lwIP netif over AWDL (the real IPv6/NDP/TCP/TLS stack)
// ----------------------------------------------------------------------------
// We create a standalone esp_netif using the Ethernet netstack so lwIP runs
// ND6/ethip6/MLD6/TCP over 14-byte-Ethernet frames, with MAC = our AWDL MAC.
// lwIP then owns fe80::EUI64 (== g_ll), auto-answers the phone's NS with NA, and
// can accept a TCP connection -- exactly what OWL delegates to the host kernel.
// TX: the netif hands us a full Ethernet frame; we strip the 14B header and wrap
// the inner payload in an AWDL data frame (build_awdl_data), window-gated so it
// leaves on ch6. RX: decapsulated AWDL data frames are handed back via
// esp_netif_receive().
// ============================================================================
struct awdl_driver_s {
  esp_netif_driver_base_t base;
};
static awdl_driver_s g_awdl_driver;
static uint16_t g_awdl_data_seq = 0;
// g_awdl_netif is declared above handle_data.

// Window-gated TX queue: lwIP transmits from the tcpip thread at any time, but a
// frame is only heard by the peer if it leaves during the ch6 window. So we enqueue
// here and drain from the cadence pass while in_ch6_window_now() -- on the cadence
// task by default, or from loop() in an AWDL_CADENCE_TASK=0 build. BUFFER, never drop
// silently (dropping stalls lwIP's ND6/TCP retransmit timers).
#ifndef AWDL_NETIF_MTU
#define AWDL_NETIF_MTU 1280
#endif
/* The knob cannot be raised into the fault it exists to prevent. esp_wifi_80211_tx refuses
   any frame over 1500 bytes, and every frame carries AWDL_TX_HEADROOM on top of the IP
   packet, so -DAWDL_NETIF_MTU=1500 would silently reinstate a 1536-byte frame that the
   driver drops without a word. The long comment in awdl_netif_init() explains the ceiling;
   this enforces it. 1460 is the true maximum; 1280 is the shipped, deliberately
   conservative value. */
static_assert(AWDL_NETIF_MTU + AWDL_TX_HEADROOM <= 1500,
              "AWDL_NETIF_MTU too large: frames would exceed esp_wifi_80211_tx's 1500-byte limit");
static_assert(AWDL_NETIF_MTU >= 1280, "IPv6 requires a link MTU of at least 1280");

/* The frame buffer is DERIVED from the link MTU, not a round number chosen once.
 * awdl_netif_transmit() refuses anything larger than AWDL_NETIF_MTU, and build_awdl_data
 * adds exactly AWDL_TX_HEADROOM on top, so this is the largest frame that can ever be
 * built -- and the static_assert above proves it stays inside the driver's 1500-byte
 * limit. Sixteen of these is the queue; at 1600 bytes a slot it was 25,632 B of internal
 * DRAM, most of it unreachable. */
struct NetifTxFrame {
  uint16_t len;
  uint8_t buf[AWDL_NETIF_MTU + AWDL_TX_HEADROOM];
};
static QueueHandle_t g_netif_txq = nullptr;

/* Raw AWDL data is neither associated STA traffic nor ordinary SoftAP traffic. The ESP
 * driver accepts it, but its completion callback reports frequent unicast failures and
 * supplies no automatic reliability usable by the netif. Retain a bounded retry count by
 * AWDL data sequence (the sequence is part of the frame and therefore survives the
 * callback). NETIF_TX_RETRY_SLOTS (64) direct-mapped entries are enough: at most 16 frames
 * are queued and a callback arrives long before 64 new netif packets can reuse a slot. Slot
 * aliasing is caught regardless -- every reader compares the FULL 16-bit sequence, not just
 * the slot index -- so a stale callback for a recycled slot is ignored, never misapplied. */
static const uint8_t NETIF_TX_RETRY_MAX = 7;
static const uint8_t NETIF_TX_RETRY_SLOTS = 64;  // > queue depth + one full drain
/* g_retry_seq / g_retry_n are touched by BOTH the tcpip task (awdl_netif_transmit arms a
   slot) and the Wi-Fi task (the TX-done callback bumps the count), deliberately WITHOUT
   g_netif_tx_mux -- unlike the in-flight gate below. Each is an aligned volatile scalar, so
   a race can neither tear a value nor corrupt memory; the worst case is one retry counted
   twice or skipped, which shifts the 7-retry bound by one. Not worth a lock. */
static volatile uint16_t g_retry_seq[NETIF_TX_RETRY_SLOTS];
static volatile uint8_t g_retry_n[NETIF_TX_RETRY_SLOTS];
static NetifTxFrame* g_retry_frame = nullptr;

/* esp_wifi_80211_tx() accepts work asynchronously. A time check around that call only
 * bounds when a frame ENTERS the driver; it does not bound when the driver's own retries
 * leave the radio. With sixteen accepted at once, a one-slot (52.5 ms usable) iPhone
 * window can close while most of the batch is still pending. macOS hid this because it
 * densified its ch6 sequence to several slots during an upload.
 *
 * Keep the old depth available as an A/B knob, but let an integration build serialize
 * netif data against the TX-done callback. Slots are keyed by AWDL sequence so immediate
 * tap traffic (which also has the AWDL data shape) cannot accidentally release or retry a
 * queued frame. */
#ifndef AWDL_NETIF_INFLIGHT_MAX
#define AWDL_NETIF_INFLIGHT_MAX 16
#endif
static_assert(AWDL_NETIF_INFLIGHT_MAX >= 1 && AWDL_NETIF_INFLIGHT_MAX <= 16,
              "AWDL_NETIF_INFLIGHT_MAX must be between 1 and the netif queue depth");
static portMUX_TYPE g_netif_tx_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t g_netif_inflight = 0;
static AwdlAck g_latest_ack{};  // Published only after successful queue admission.
static uint32_t g_ack_skipped = 0, g_tx_queue_drops = 0;

static bool netif_ack_obsolete(const uint8_t* frame, size_t len) {
  AwdlAck ack;
  if (len < AWDL_TX_HEADROOM || !awdl_ack_parse(frame + AWDL_TX_HEADROOM, len - AWDL_TX_HEADROOM, &ack)) return false;
  portENTER_CRITICAL(&g_netif_tx_mux);
  const bool obsolete = awdl_ack_supersedes(&g_latest_ack, &ack);
  if (obsolete) ++g_ack_skipped;
  portEXIT_CRITICAL(&g_netif_tx_mux);
  return obsolete;
}

static volatile uint16_t g_netif_inflight_seq[NETIF_TX_RETRY_SLOTS];
static volatile uint8_t g_netif_inflight_used[NETIF_TX_RETRY_SLOTS];

static bool netif_inflight_room(void) {
  bool room;
  portENTER_CRITICAL(&g_netif_tx_mux);
  room = g_netif_inflight < AWDL_NETIF_INFLIGHT_MAX;
  portEXIT_CRITICAL(&g_netif_tx_mux);
  return room;
}

static bool netif_inflight_begin(uint16_t seq) {
  const uint8_t slot = (uint8_t)(seq & (NETIF_TX_RETRY_SLOTS - 1));
  bool admitted = false;
  portENTER_CRITICAL(&g_netif_tx_mux);
  if (g_netif_inflight < AWDL_NETIF_INFLIGHT_MAX && !g_netif_inflight_used[slot]) {
    g_netif_inflight_used[slot] = 1;
    g_netif_inflight_seq[slot] = seq;
    g_netif_inflight++;
    admitted = true;
  }
  portEXIT_CRITICAL(&g_netif_tx_mux);
  return admitted;
}

static bool netif_inflight_done(uint16_t seq) {
  const uint8_t slot = (uint8_t)(seq & (NETIF_TX_RETRY_SLOTS - 1));
  bool matched = false;
  portENTER_CRITICAL(&g_netif_tx_mux);
  if (g_netif_inflight_used[slot] && g_netif_inflight_seq[slot] == seq) {
    g_netif_inflight_used[slot] = 0;
    if (g_netif_inflight) g_netif_inflight--;
    matched = true;
  }
  portEXIT_CRITICAL(&g_netif_tx_mux);
  return matched;
}

/* esp_wifi_80211_tx() returning ESP_OK means only that the driver accepted the raw
 * frame. On a failed unicast completion, put the exact frame back at the HEAD of the
 * window-gated queue. TCP duplicates are safe; losing one segment and waiting for lwIP's
 * exponential RTO is what produced the observed 45-second application stall.
 *
 * This runs in the Wi-Fi task. It never waits or logs. The one expensive operation is the
 * queue's bounded frame copy, paid only after a failed TX; successful completions are a
 * few increments.
 *
 * THE IN-FLIGHT GATE ASSUMES ONE COMPLETION PER ACCEPTED FRAME. Every esp_wifi_80211_tx()
 * that returned ESP_OK is expected to reach this callback exactly once, releasing its slot
 * via netif_inflight_done(); a refused injection never calls back and is released at the
 * injection site instead (awdl_netif_flush_tx). Were the driver to silently drop a
 * completion for an accepted frame, that slot would leak, and once AWDL_NETIF_INFLIGHT_MAX
 * such leaks accumulated the gate would stop admitting for the rest of the session. Measured
 * on this part the ratio was 1:1, so no reaper is carried. */
static void raw_tx_done_cb(const esp_80211_tx_info_t* ti) {
  if (!ti || !ti->data) return;
  const uint8_t* f = ti->data;
  const bool ok = (ti->tx_status == WIFI_SEND_SUCCESS);
  const uint8_t type = f[0] & 0x0c;
  if (type == 0x08) {
    const bool mcast = (f[4] & 0x01) != 0;
    if (mcast) return;  // multicast has no link-layer ACK

    /* Only our AWDL/IPv6 netif shape. This excludes hidden-AP traffic and makes every
       offset below guarded: LLC ends at 31, AWDL seq is 34..35, IPv6 starts at 40. */
    if (!g_netif_txq || f[32] != 0x03 || f[33] != 0x04 || (f[40] >> 4) != 6) return;
    const uint16_t seq = (uint16_t)f[34] | ((uint16_t)f[35] << 8);
    /* A callback from the immediate RX-tap path has the same wire shape but was never
       admitted by the queued netif. Do not let it release the gate or enter retry. */
    if (!netif_inflight_done(seq)) return;
    if (g_cadence_task) xTaskNotifyGive(g_cadence_task);
    if (ok) return;
    const uint8_t slot = (uint8_t)(seq & (NETIF_TX_RETRY_SLOTS - 1));
    if (g_retry_seq[slot] != seq || g_retry_n[slot] >= NETIF_TX_RETRY_MAX) {
      return;
    }
    /* Length comes from the IPv6 payload field, NOT ti->data_len: that field is a uint8_t
       and would truncate any frame past 255 bytes. f[44..45] is the IPv6 payload length;
       total is the whole 802.11 frame (40B link header + 40B IPv6 header + payload). */
    const uint16_t payload = ((uint16_t)f[44] << 8) | f[45];
    const size_t total = AWDL_TX_HEADROOM + 40u + payload;
    if (total > AWDL_NETIF_MTU + AWDL_TX_HEADROOM) {
      return;
    }
    if (netif_ack_obsolete(f, total)) return;
    if (!g_retry_frame) {
      return;
    }
    NetifTxFrame& retry = *g_retry_frame;  // Wi-Fi task is the callback's single writer
    retry.len = (uint16_t)total;
    memcpy(retry.buf, f, total);
    g_retry_n[slot]++;
    if (xQueueSendToFront(g_netif_txq, &retry, 0) == pdTRUE) {
      if (g_cadence_task) xTaskNotifyGive(g_cadence_task);
    }
  }
}

// Wrap a raw IPv6 packet (lwIP Ethernet payload) in an AWDL data frame:
// 802.11 data(24) + LLC/SNAP(8) + awdl_data hdr(8, seq++) + IPv6. addr1 = the
// Ethernet dst lwIP chose (peer unicast or a 33:33 multicast).
/* Write the 40-byte link header into the front of a buffer whose IPv6 payload already
 * sits at buf + AWDL_TX_HEADROOM. Same bytes build_awdl_data() emits, no copy. */
static void build_awdl_data_at(uint8_t* buf, const uint8_t* dst_mac, uint16_t ethertype, int iplen, uint16_t seq) {
  (void)iplen;
  uint8_t* p = buf;
  *p++ = 0x08;
  *p++ = 0x00;
  *p++ = 0x00;
  *p++ = 0x00;
  memcpy(p, dst_mac, 6);
  p += 6;
  memcpy(p, g_awdl_mac, 6);
  p += 6;
  memcpy(p, AWDL_BSSID, 6);
  p += 6;
  *p++ = 0x00;
  *p++ = 0x00;
  static const uint8_t llc[8] = {0xaa, 0xaa, 0x03, 0x00, 0x17, 0xf2, 0x08, 0x00};
  memcpy(p, llc, 8);
  p += 8;
  *p++ = 0x03;
  *p++ = 0x04;
  *p++ = seq & 0xff;
  *p++ = (seq >> 8) & 0xff;
  *p++ = 0x00;
  *p++ = 0x00;
  *p++ = (ethertype >> 8) & 0xff;
  *p++ = ethertype & 0xff;
  /* p - buf == AWDL_TX_HEADROOM: 24 bytes of 802.11 (FC and duration 4, three addresses
     18, sequence 2), 8 of LLC/SNAP, 8 of awdl_data. AWDL_TX_HEADROOM is public -- a caller
     places its IPv6 packet at that offset and hands the buffer to awdl_tx_ip6_now() -- so
     the constant in awdl_port_esp32.h and the bytes written here move together. */
}

/* One header builder, not two. This used to write the same forty bytes a second time,
   by hand, beside build_awdl_data_at() -- so a change to the AWDL data header had to be
   made in two places and would go unnoticed in one. */
static int build_awdl_data(uint8_t* out, const uint8_t* dst_mac, uint16_t ethertype, const uint8_t* ip, int iplen,
                           uint16_t seq) {
  build_awdl_data_at(out, dst_mac, ethertype, iplen, seq);
  memcpy(out + AWDL_TX_HEADROOM, ip, (size_t)iplen);
  return AWDL_TX_HEADROOM + iplen;
}

static esp_err_t awdl_netif_transmit(void* h, void* buffer, size_t len) {
  (void)h;
  if (len < 14 || !g_netif_txq) return ESP_OK;
  const uint8_t* e = (const uint8_t*)buffer;
  uint16_t ethertype = (e[12] << 8) | e[13];
  if (ethertype != 0x86dd) return ESP_OK;  // v6-only interface
  int iplen = (int)len - 14;
  /* 1460, not 1500: build_awdl_data adds AWDL_TX_HEADROOM, and esp_wifi_80211_tx refuses
     the result over 1500. At the shipped MTU nothing reaches this, but a raised knob or a
     future path would otherwise hand the driver a frame it silently drops. Counted, because
     the queue above promises never to drop one silently. */
  if (iplen <= 0) return ESP_OK;
  /* The same constant NetifTxFrame is sized from, so the buffer provably cannot overflow.
     lwIP will not hand down more than the link MTU it was given, so this counts something
     that should never happen rather than something expected -- and ovsz= in the STAT line
     is where it would show up. */
  if (iplen > AWDL_NETIF_MTU) {
    return ESP_OK;
  }
  static NetifTxFrame tf;  // tcpip thread is single -> safe
  const uint16_t seq = g_awdl_data_seq++;
  tf.len = build_awdl_data(tf.buf, e, ethertype, e + 14, iplen, seq);
  const uint8_t retry_slot = (uint8_t)(seq & (NETIF_TX_RETRY_SLOTS - 1));
  g_retry_seq[retry_slot] = seq;
  g_retry_n[retry_slot] = 0;
  const bool queued = xQueueSend(g_netif_txq, &tf, 0) == pdTRUE;
  AwdlAck ack;
  const bool plainAck = queued && awdl_ack_parse(e + 14, iplen, &ack);
  portENTER_CRITICAL(&g_netif_tx_mux);
  if (!queued) ++g_tx_queue_drops;
  if (plainAck) g_latest_ack = ack;
  portEXIT_CRITICAL(&g_netif_tx_mux);
  // Wake the cadence so a queued frame does not wait for the next scheduled instant.
  // Guarded: lwIP transmits ND6/MLD the moment the netif comes up, and an
  // AWDL_CADENCE_TASK=0 build has no task to notify -- an unguarded xTaskNotifyGive(NULL)
  // is a configASSERT boot loop.
  if (g_cadence_task) xTaskNotifyGive(g_cadence_task);
  return ESP_OK;
}

// Drain the netif TX queue onto the air while we're inside the ch6 window.
// `deadline_us` is absolute and is re-tested BEFORE every frame, not once at entry. The
// budget alone is not a limit: a tick that tests room for ONE worst-case injection and
// then grants sixteen has granted 53.7 ms at the measured p90 cost of 3,354 us, against a
// 52,536 us run -- one admission would consume the whole window, in the middle of the
// transfer that made the queue deep in the first place. The pass-start clock cannot be
// trusted either, because the MIF and any announcement riding it have already spent part
// of the window before the drain begins.
static void awdl_netif_flush_tx(int budget, int64_t deadline_us) {
  if (!g_netif_txq || budget <= 0) return;
  static NetifTxFrame tf;
  // Window-gated, and it stays that way -- this was A/B'd on hardware three ways
  // with ping6 over awdl0 (25 probes per run) plus the TLS handshake time:
  //   window-gated only : 0.0% loss, RTT min 11.4 / avg 488.5 / max 968.4 ms, hs 1042-1069ms
  //   immediate only    : 8.0% loss, RTT min  6.6 / avg 430.0 / max 942.7 ms, hs 2133ms,
  //                       and TLS handshake failures rose from 1/190s to 4/30s
  //   both copies       : 4.0% loss, RTT min  8.5 / avg 444.5 / max 952.7 ms, hs 1042ms
  // Sending early does NOT speed anything up, which is the useful finding: the peer
  // really is not listening outside its ch6 dwell, so an early copy is simply thrown
  // away, and the handshake time is unmoved. Gating is correct.
  //
  // Where the ~500ms per leg actually goes: the RTT distribution stayed uniform over
  // 0..1048ms in every variant, including the one where our side answered instantly.
  // macOS queues its OWN awdl0 transmissions for its next window too, so half the
  // round trip belongs to the peer's scheduler. The lever that WOULD help is ch6
  // densification (the mesh was observed going from 1/16 to 5/16 ch6 slots under
  // traffic), and that is the peer's decision, not ours.
  while (budget-- > 0 && esp_timer_get_time() < deadline_us && in_ch6_window_now() && netif_inflight_room() &&
         xQueueReceive(g_netif_txq, &tf, 0) == pdTRUE) {
    if (netif_ack_obsolete(tf.buf, tf.len)) continue;
    const uint16_t seq = (uint16_t)tf.buf[34] | ((uint16_t)tf.buf[35] << 8);
    /* There is only one queue consumer. Failure here therefore means a defensive slot
       collision, not ordinary contention; preserve the frame rather than dropping it. */
    if (!netif_inflight_begin(seq)) {
      (void)xQueueSendToFront(g_netif_txq, &tf, 0);
      break;
    }
    esp_err_t te = esp_wifi_80211_tx(WIFI_IF_AP, tf.buf, tf.len, true);
    if (te != ESP_OK) netif_inflight_done(seq);  // refused injections have no completion callback
  }
}

static void awdl_netif_rx_free(void* h, void* buffer) {
  (void)h;
  free(buffer);  // free(NULL) is a safe no-op
}

/* Runs on the tcpip thread via esp_netif_tcpip_exec(), which is the only context where
   touching the lwIP netif is legal here -- see the block in awdl_netif_init() for why. */
static esp_err_t awdl_netif_set_mtu_cb(void* ctx) {
  struct netif* ln = (struct netif*)esp_netif_get_netif_impl(g_awdl_netif);
  if (!ln) return ESP_ERR_INVALID_STATE;
  const uint16_t m = (uint16_t)(uintptr_t)ctx;
  ln->mtu = m;
#if LWIP_IPV6
  ln->mtu6 = m; /* the one IPv6 TCP actually reads */

#endif
  return ESP_OK;
}

static esp_err_t awdl_netif_post_attach(esp_netif_t* netif, void* h) {
  awdl_driver_s* d = (awdl_driver_s*)h;
  d->base.netif = netif;
  esp_netif_driver_ifconfig_t ifcfg = {};
  ifcfg.handle = d;
  ifcfg.transmit = awdl_netif_transmit;
  ifcfg.driver_free_rx_buffer = awdl_netif_rx_free;  // esp_pbuf_free calls this; MUST be non-null
  return esp_netif_set_driver_config(netif, &ifcfg);
}

static esp_err_t awdl_netif_init(void) {
  g_netif_txq = xQueueCreateWithCaps(16, sizeof(NetifTxFrame),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);  // 16 x (MTU+headroom); holds a full TLS
                                                                            // ServerHello flight burst between
                                                                            // ch6 windows without dropping frames
  if (!g_netif_txq) return ESP_ERR_NO_MEM;
  /* A failed-frame copy is cold diagnostic/recovery storage, not timing-critical working
     memory. Prefer PSRAM so the extra reliability does not take another frame-sized bite
     out of the contiguous internal arena mbedTLS needs; fall back for ESP32 boards with
     no PSRAM. The queue operation copies it before the callback returns. */
  g_retry_frame = (NetifTxFrame*)heap_caps_malloc(sizeof(NetifTxFrame), MALLOC_CAP_SPIRAM);
  if (!g_retry_frame) g_retry_frame = (NetifTxFrame*)malloc(sizeof(NetifTxFrame));
  memset((void*)g_retry_seq, 0, sizeof g_retry_seq);
  memset((void*)g_retry_n, 0, sizeof g_retry_n);
  memset((void*)g_netif_inflight_seq, 0, sizeof g_netif_inflight_seq);
  memset((void*)g_netif_inflight_used, 0, sizeof g_netif_inflight_used);
  g_netif_inflight = 0;
  g_latest_ack = {};
  g_ack_skipped = g_tx_queue_drops = 0;
  static esp_netif_inherent_config_t base = {};
  base.flags =
      (esp_netif_flags_t)(ESP_NETIF_FLAG_AUTOUP | ESP_NETIF_FLAG_MLDV6_REPORT | ESP_NETIF_FLAG_EVENT_IP_MODIFIED);
  memcpy(base.mac, g_awdl_mac, 6);
  base.if_key = "AWDL";
  base.if_desc = "awdl0";
  base.route_prio = 1;
  esp_netif_config_t cfg = {};
  cfg.base = &base;
  cfg.driver = nullptr;
  cfg.stack = ESP_NETIF_NETSTACK_DEFAULT_ETH;
  g_awdl_netif = esp_netif_new(&cfg);
  if (!g_awdl_netif) return ESP_ERR_NO_MEM;
  g_awdl_driver.base.post_attach = awdl_netif_post_attach;
  ignore_state(esp_netif_attach(g_awdl_netif, &g_awdl_driver));
  ignore_state(esp_netif_set_mac(g_awdl_netif, g_awdl_mac));
  // bring the interface + link up, then create the fe80::EUI64 (== g_ll)
  esp_netif_action_start(g_awdl_netif, nullptr, 0, nullptr);
  esp_netif_action_connected(g_awdl_netif, nullptr, 0, nullptr);
  ignore_state(esp_netif_create_ip6_linklocal(g_awdl_netif));

  /* CAP THE LINK MTU. Left at esp_netif's Ethernet default of 1500, sending stalls, and
   * the reason is a hard limit inside the WiFi driver:
   *
   *   ieee80211_raw_frame_sanity_check+0x79   movi.n a6,23  ; bgeu a6,a4 -> refuse
   *                                  +0x7e    movi   a6,0x5dc (1500) ; blt a6,a4 -> refuse
   *                                  +0xac    movi   a2,0x102 (ESP_ERR_INVALID_ARG)
   *
   * so esp_wifi_80211_tx accepts 24..1500 and refuses anything longer -- the same range
   * esp_wifi.h:1191 documents. At mtu 1500 lwIP's send MSS is 1436, and a full-MSS segment
   * becomes 1436 + 40 (IPv6) + 20 (TCP) + 40 (our 802.11/LLC/awdl_data headroom)
   * = a 1536-byte frame, which is over the limit and is dropped with no log at all
   * (CONFIG_ESP_WIFI_DEBUG_PRINT is unset, so only the return value ever says so).
   * Measured: every stalled upload refused exactly those frames, unicast to the peer's own
   * MAC, next-header TCP.
   *
   * ⚠️ TWO THINGS MAKE THIS SUBTLE, and getting either wrong looks like a hang.
   *
   * 1. IT MUST RUN ON THE TCPIP THREAD. lwIP is built with CONFIG_LWIP_CHECK_THREAD_SAFETY
   *    and CONFIG_LWIP_ESP_LWIP_ASSERT, so touching a netif from another task -- calling
   *    netif_get_by_index() straight from the caller's, say -- trips
   *    LWIP_ASSERT_CORE_LOCKED() -> __assert_func -> abort -> reboot. On this board that
   *    reads as a HANG rather than a panic, because the panic goes to UART0 while the
   *    console is TinyUSB CDC: nothing is printed, and the USB re-enumeration ends the
   *    capture. esp_netif_tcpip_exec() runs the callback on the tcpip thread, where the
   *    lock is held.
   *
   * 2. IPv6 READS mtu6, NOT mtu. tcp_eff_send_mss_netif -> nd6_get_destination_mtu takes
   *    netif->mtu6; netif_add merely seeds it from mtu at creation. Writing mtu alone
   *    changes nothing for the TCP that matters here. Writing BOTH also bounds any router
   *    advertisement, because nd6_input stores min(RA_mtu, netif->mtu, mtu6).
   *
   * 1280 is not the driver's limit (that would be 1460 of IPv6 payload); it is the minimum
   * MTU IPv6 guarantees and the AWDL path MTU. It gives a send MSS of 1220 and frames of
   * at most 1320 bytes, leaving 180 bytes of headroom against the real ceiling -- which is
   * the point. */
  (void)esp_netif_tcpip_exec(awdl_netif_set_mtu_cb, (void*)(uintptr_t)AWDL_NETIF_MTU);

  return ESP_OK;
}
// ============================================================================
// WiFi bring-up
// ============================================================================
static void ignore_state(esp_err_t e) { (void)e; }
/* Bring the radio up promiscuous on ch6 and hand every management and data frame to
 * sniffer_cb. The hidden zero-client AP exists only because the driver will not transmit
 * at all without an interface in a transmitting mode; nothing ever associates to it, and
 * its 60 s beacon interval keeps it off the air in between.
 *
 * NOT ONE OF THESE IS AN ESP_ERROR_CHECK. That macro panics the chip, which is a
 * defensible choice for a project's own firmware -- fail loudly on a bench -- and an
 * indefensible one in a library, where the boot being ended belongs to someone else's
 * product. All eleven return, and awdl_begin() reports the first failure. */
static esp_err_t wifi_probe_init(void) {
  esp_err_t e;
  ignore_state(nvs_flash_init());
  ignore_state(esp_netif_init());
  ignore_state(esp_event_loop_create_default());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  if ((e = esp_wifi_init(&cfg)) != ESP_OK) return e;
  if ((e = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return e;
  if ((e = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK) return e;
  /* The interface MAC and the addr1/addr2 used by AWDL MUST be the same address.
   * Merely deriving a locally-administered g_awdl_mac while leaving the radio at its
   * factory AP MAC makes frames addressed to our AWDL identity invisible to the
   * hardware ACK engine. The promiscuous callback still sees and passes them to lwIP,
   * which hides the mismatch functionally, but the peer receives no 802.11 ACK and
   * retries every inbound TCP frame. Set it while the AP interface is still disabled,
   * as esp_wifi_set_mac requires; wifi_start below enables it. */
  if ((e = esp_wifi_get_mac(WIFI_IF_AP, g_awdl_mac)) != ESP_OK) return e;
  g_awdl_mac[0] = (g_awdl_mac[0] | 0x02) & 0xfe;
  if ((e = esp_wifi_set_mac(WIFI_IF_AP, g_awdl_mac)) != ESP_OK) return e;
  wifi_config_t ap = {};
  strcpy((char*)ap.ap.ssid, "esp32drop-probe"); /* the library is ESP32Drop now */
  ap.ap.channel = PROBE_CHANNEL;
  ap.ap.authmode = WIFI_AUTH_OPEN;
  ap.ap.ssid_hidden = 1;
  ap.ap.max_connection = 0;
  ap.ap.beacon_interval = 60000;
  if ((e = esp_wifi_set_config(WIFI_IF_AP, &ap)) != ESP_OK) return e;
  if ((e = esp_wifi_start()) != ESP_OK) return e;
  if ((e = esp_wifi_register_80211_tx_cb(raw_tx_done_cb)) != ESP_OK) return e;
  if ((e = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK) return e;
  wifi_promiscuous_filter_t filt = {
      .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
  };
  if ((e = esp_wifi_set_promiscuous_filter(&filt)) != ESP_OK) return e;
  if ((e = esp_wifi_set_promiscuous_rx_cb(sniffer_cb)) != ESP_OK) return e;
  if ((e = esp_wifi_set_promiscuous(true)) != ESP_OK) return e;
  if ((e = esp_wifi_set_channel(PROBE_CHANNEL, WIFI_SECOND_CHAN_NONE)) != ESP_OK) return e;
  return ESP_OK;
}
static bool awdl_win_phase(const struct AwdlWinState* w, int64_t* out) {
  if (!w->have_lock) return false;
  struct AwdlPll s;
  awdl_pll_init(&s);
  s.phase = w->phase;
  s.freq = w->freq;
  s.tlast = w->tlast;
  *out = awdl_pll_cycle_phase(&s, (uint32_t)(esp_timer_get_time() - g_ts_off32));
  return true;
}
static bool in_ch6_window_now() {
  struct AwdlWinState w;
  awdl_win_read(&w);
  int64_t ph;
  if (!awdl_win_phase(&w, &ph)) return false;
  struct AwdlWindow win;
  awdl_window_init(&win, w.ch6_mask);
  return awdl_window_open_at(&win, ph);
}
// One pass of the cadence. Pure decision from awdl_tick.h, then the four duties in the one
// order that is safe: elect (so the MIF has a fresh master), MIF, announce riding it, then
// the drain -- which must come last so sixteen netif frames can never be queued ahead of
// the frame that keeps us visible.
void awdl_cadence_pass(void) {
  // The election runs BEFORE the tick, not after. Reading master_ok from a pre-election
  // snapshot costs the first frame after every master re-acquisition a whole 50 ms
  // election period, and in the other direction lets the tick book an instant as sent --
  // advancing mif_k and pushing the announce out 2.5 s -- while the caller then suppresses
  // the send because the fresh election came back empty.
  const int64_t now_us = esp_timer_get_time();
  if (awdl_tick_elect_due(&g_tick, now_us)) awdl_elect_and_publish();

  struct AwdlWinState w;
  Selection sel;
  awdl_pub_read(&w, &sel);

  /* The CURRENT isolation episode, not a lifetime total. Tracked here rather than inside
     awdl_status_read() because a status accessor that only measures while somebody is
     asking is not measuring. */
  if (sel.idx < 0) {
    if (!g_no_master) {
      g_no_master = true;
      g_no_master_since_ms = (uint32_t)(now_us / 1000);
    }
  } else
    g_no_master = false;

  struct AwdlTickIn in;
  in.now_us = now_us;
  in.have_lock = w.have_lock;
  in.ch6_mask = w.ch6_mask;
  in.master_ok = (sel.idx >= 0);
  in.phase_us = 0;
  if (w.have_lock) {
    struct AwdlPll p;
    awdl_pll_init(&p);
    p.phase = w.phase;
    p.freq = w.freq;
    p.tlast = w.tlast;
    in.phase_us = awdl_pll_cycle_phase(&p, (uint32_t)(in.now_us - (int64_t)g_ts_off32));
  }

  struct AwdlTickOut out;
  awdl_tick_step(&g_tick, &in, &out);  // out.do_elect is already honoured above

  // sel is post-election, so do_mif and sel.idx >= 0 cannot disagree.
  if (out.do_mif) {
    send_mif(sel);
    // The announcement itself belongs to the layer above, through the registered
    // callback. Nested inside do_mif: an announcement rides a window we have already
    // decided to transmit in.
    if (out.do_announce && g_announce_cb) {
      // The same in-callback flag the RX tap sets. Without it awdl_tx_ip6_now() refuses
      // the announcement, and the failure is silent in every health indicator: late=0,
      // locked=1, masters=2, all green, and mdns_tx frozen at zero. A badge that is
      // perfectly synchronised and advertises nothing is invisible; this counter is the
      // only thing that says so.
      g_in_tap = true;
      g_announce_cb(g_announce_ctx);
      g_in_tap = false;
    }
  }
  if (out.drain_max) awdl_netif_flush_tx(out.drain_max, out.drain_until_us);
  g_cadence_wake_us = out.next_wake_us;
}

#if AWDL_CADENCE_TASK
static void awdl_cadence_task_fn(void*) {
  for (;;) {
    /* BEFORE the pass, never during it: a pass is what puts a frame on the air. */
    if (g_awdl_stop) {
      g_cad_stopped = true;
      vTaskDeleteWithCaps(nullptr);
    }
    awdl_cadence_pass();
    int64_t dt = g_cadence_wake_us - esp_timer_get_time();
    // At configTICK_RATE_HZ 1000 one tick is 1 ms, so the sleep quantises the schedule to
    // 1 ms -- which is the worst wake-jitter cell tools/test-tick.sh exercises, and the
    // anchored schedule delivers every instant in it. Never a constant: a relative tick is
    // a sleep that does not know where the window is.
    TickType_t ticks = (dt <= 0) ? 1 : (TickType_t)((dt + 999) / 1000);
    if (ticks == 0) ticks = 1;
    ulTaskNotifyTake(pdTRUE, ticks);
  }
}
#endif

/* ==========================================================================
 * The seam: the port's whole public surface, and no more of it. Without these
 * accessors, fifty-six file-scope symbols would cross this line instead.
 * ========================================================================== */

esp_netif_t* awdl_netif(void) { return g_awdl_netif; }

void awdl_identity(struct AwdlIdentity* out) {
  memcpy(out->mac, g_awdl_mac, 6);
  memcpy(out->ll, g_ll, 16);
  strlcpy(out->instance, g_instance, sizeof(out->instance));
  strlcpy(out->pair_sid, g_pair_sid, sizeof(out->pair_sid));
}

uint32_t awdl_cadence_stack_free_words(void) {
  return g_cadence_task ? (uint32_t)uxTaskGetStackHighWaterMark(g_cadence_task) : 0;
}

/* --- bring-up -------------------------------------------------------------
 *
 * The ORDER is load-bearing in two places and both are commented where they happen: the
 * cadence task is created BEFORE the netif, because lwIP transmits ND6/MLD the instant the
 * interface comes up and awdl_netif_transmit notifies that task; and the published window
 * and selection are initialised before either task can read them, because BSS zero is not
 * "empty" -- slot 0 is a real slot and row 0 is a real row.
 *
 * Every step returns rather than aborting. ESP_ERROR_CHECK panics the chip: a defensible
 * choice for a project's own firmware and an indefensible one for a library, because a
 * user's boot is not ours to end. */
esp_err_t awdl_begin(void) {
  esp_err_t e;
  if (g_ring) return ESP_ERR_INVALID_STATE;
  g_ring =
      static_cast<RawFrame*>(heap_caps_malloc(sizeof(RawFrame) * RING_SLOTS, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!g_ring) return ESP_ERR_NO_MEM;

  awdl_ring_init(&g_ring_cur, RING_SLOTS);
  awdl_winstate_init(&g_win);
  g_sel_pub.idx = -1;
  /* OUTSIDE any cadence-task guard, and that is the whole point: g_tick is BSS, so an
   * AWDL_CADENCE_TASK=0 build that skipped this would leave mif_period_us at 0, which
   * makes awdl_tick_step's late-instant loop advance by zero and spin forever, silently,
   * on the first pass with a window open -- the escape hatch would itself be a hang. */
  awdl_tick_init(&g_tick, nullptr);

  if ((e = wifi_probe_init()) != ESP_OK) return e;

  /* wifi_probe_init() derived the locally-administered AWDL identity and installed that
     exact address on WIFI_IF_AP before enabling it, so the hardware can ACK frames sent
     to the same MAC that the software path advertises. */
  snprintf(g_instance, sizeof(g_instance), "%02x%02x%02x%02x%02x%02x", g_awdl_mac[0], g_awdl_mac[1], g_awdl_mac[2],
           g_awdl_mac[3], g_awdl_mac[4], g_awdl_mac[5]);
  /* UUID-shaped session id derived from the MAC, for the pairing TXT. */
  snprintf(g_pair_sid, sizeof(g_pair_sid), "%02X%02X%02X%02X-%02X%02X-4D5E-8F00-%02X%02X%02X%02X%02X%02X",
           g_awdl_mac[0], g_awdl_mac[1], g_awdl_mac[2], g_awdl_mac[3], g_awdl_mac[4], g_awdl_mac[5], g_awdl_mac[0],
           g_awdl_mac[1], g_awdl_mac[2], g_awdl_mac[3], g_awdl_mac[4], g_awdl_mac[5]);
  /* fe80::EUI-64 of that MAC. Derived here, synchronously, and never taken from lwIP's
   * asynchronous link-local: the reverse order opens a boot window in which the identity
   * is zeros. The IDENT line in the diagnostics example VERIFIES the two agree. */
  /* One derivation, used for ours and for every peer's -- see awdl_ll_of_mac(). Two
     copies of this arithmetic is how a sender comes to address a peer at an fe80 that
     is one bit away from the right one, and that failure looks like a dead link. */
  awdl_ll_of_mac(g_awdl_mac, g_ll);

  /* Frame path on core 1 at priority 3; the WiFi RX callback stays undisturbed on core 0. */
  if (xTaskCreatePinnedToCoreWithCaps(proc_task, "awdl_proc", 8192, nullptr, 3, nullptr, 1,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    return ESP_ERR_NO_MEM;

#if AWDL_CADENCE_TASK
  /* BEFORE awdl_netif_init(), see the note above. Priority 2 from a priority-1 caller
   * yields immediately, so the first cadence pass runs before this function returns --
   * which closes the window in which the published state is still its zeroed self. */
  if (xTaskCreatePinnedToCoreWithCaps(awdl_cadence_task_fn, "awdl_cad", 8192, nullptr, 2, &g_cadence_task, 1,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    return ESP_ERR_NO_MEM;
#endif

  return awdl_netif_init();
}

/* Bring the link down and hand the memory back. The inverse of awdl_begin(), and a
 * begin() after it must work.
 *
 * WHY A LIBRARY NEEDS THIS. A send-only device cannot hold AWDL and BLE at once and still
 * have anything left for the application: measured, all three residents (AWDL, the
 * mbedTLS context, the BLE controller) leave 2,956 B of internal RAM, while taking turns
 * leaves 25,424 B. Taking turns needs a way to put AWDL down, and there was none -- so a
 * sender got exactly one wake per boot, and missing the peer's open window meant a
 * reboot.
 *
 * ORDER, and each step is here because the reverse breaks something:
 *   1. the tasks, cooperatively -- they must be gone before anything they touch is freed
 *   2. the radio, before the netif -- so no frame arrives for an interface that is gone
 *   3. the netif and its 25.6 KB transmit queue
 * Steps that awdl_begin() performs with ignore_state() -- nvs_flash_init,
 * esp_netif_init, esp_event_loop_create_default -- are NOT undone. They are process-wide
 * one-time inits that other components share; tearing them down would break whoever else
 * initialised them, and begin() already tolerates finding them present.
 *
 * Returns ESP_ERR_INVALID_STATE if the link was not up, or if a task did not acknowledge
 * within the timeout -- in which case NOTHING is freed. A half-torn-down link that
 * reports success is the failure this whole function exists to avoid. */
esp_err_t awdl_end(void) {
  if (!g_awdl_netif) return ESP_ERR_INVALID_STATE;

  /* 1. Ask the tasks to leave, and wait for both. The cadence task sleeps on a
   *    notification, so poke it rather than waiting out its timer. */
  g_proc_stopped = g_cad_stopped = false;
  g_awdl_stop = true;
  if (g_cadence_task) xTaskNotifyGive(g_cadence_task);
#if AWDL_CADENCE_TASK
  const bool want_cad = true;
#else
  const bool want_cad = false;
#endif
  /* One availability window is 1,048,576 us and proc_task's own idle sleep is one tick,
     so a second is two orders of magnitude of slack. If they have not answered by then
     something is wrong that freeing memory would only make worse. */
  for (int i = 0; i < 1000; i++) {
    if (g_proc_stopped && (!want_cad || g_cad_stopped)) break;
    vTaskDelay(1);
  }
  if (!g_proc_stopped || (want_cad && !g_cad_stopped)) {
    g_awdl_stop = false; /* leave the link running rather than half-dead */
    return ESP_ERR_INVALID_STATE;
  }
  g_cadence_task = nullptr;

  /* 2. The radio. Promiscuous off FIRST: sniffer_cb writes into the frame ring from the
   *    Wi-Fi task, and proc_task is already gone, so every frame after this point is work
   *    nobody will ever collect. */
  // A failure must retain callback storage; the caller performs recovery.
  esp_err_t error;
  if ((error = esp_wifi_set_promiscuous(false)) != ESP_OK) return error;
  if ((error = esp_wifi_set_promiscuous_rx_cb(nullptr)) != ESP_OK) return error;
  if ((error = esp_wifi_register_80211_tx_cb(nullptr)) != ESP_OK) return error;
  if ((error = esp_wifi_stop()) != ESP_OK) return error;
  if ((error = esp_wifi_deinit()) != ESP_OK) return error;
  heap_caps_free(g_ring);
  g_ring = nullptr;

  /* 3. The netif, then its queue. NetifTxFrame is stored BY VALUE, so vQueueDelete frees
   *    every queued frame with it -- there is nothing to drain. */
  esp_netif_action_disconnected(g_awdl_netif, nullptr, 0, nullptr);
  esp_netif_action_stop(g_awdl_netif, nullptr, 0, nullptr);
  esp_netif_destroy(g_awdl_netif);
  g_awdl_netif = nullptr;
  if (g_netif_txq) {
    vQueueDeleteWithCaps(g_netif_txq);
    g_netif_txq = nullptr;
  }
  if (g_retry_frame) {
    free(g_retry_frame);
    g_retry_frame = nullptr;
  }

  /* 4. State a later begin() must not inherit. The rings, the window and the tick are
   *    re-initialised by begin() itself; what it does NOT reset is the ND6 seed table,
   *    whose entries name peers this link no longer has. */
  g_nseeded = 0;
  g_awdl_stop = false;
  return ESP_OK;
}

void awdl_transport_stats(AwdlTransportStats* out) {
  AwdlWinState window;
  awdl_win_read(&window);
  out->locked = window.have_lock;
  out->channelMask = window.ch6_mask;
  out->ringDrops = g_ring_cur.drops;
  out->queued = g_netif_txq ? uxQueueMessagesWaiting(g_netif_txq) : 0;
  portENTER_CRITICAL(&g_netif_tx_mux);
  out->inflight = g_netif_inflight;
  out->acksSkipped = g_ack_skipped;
  out->txQueueDrops = g_tx_queue_drops;
  portEXIT_CRITICAL(&g_netif_tx_mux);
}
