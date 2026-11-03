/* awdl_diag.h -- the AWDL layer's counters, in one object.
 *
 * These are NOT the public API. awdl_status.h is: a small, curated set with the derived
 * questions already answered. This is the raw instrument panel, for the diagnostics
 * example and for the soak analysis in tools/ -- the numbers you want when something is
 * wrong and you do not yet know what.
 *
 * They live in ONE struct rather than as thirty-odd file-scope globals, and that is a
 * boundary decision before it is a tidiness one. As separate symbols -- written by the
 * AWDL layer, read by the caller's status line -- they make the seam between the two look
 * fifty symbols wide when the functional seam is about ten. The rest is the instrument
 * panel reaching through the wall, and a library cannot be extracted across a seam that
 * wide.
 *
 * Everything here is written by exactly one task and read by another, so the fields are
 * volatile where they are counters. None of them is load-bearing: losing an increment
 * costs a diagnostic, never a frame.
 */
#pragma once
#include <stdint.h>

/* THE DETAIL SET IS OPT-IN, and the default is off.
 *
 * A few of the instruments below are not paid for by what they compute but by WHERE they
 * run. Two sit on the frame path; one of those two sits INSIDE the g_mux critical section,
 * which the election and the caller's status line both queue behind; and one sits between
 * build_mif() and esp_wifi_80211_tx(), so it delays the very transmission whose placement
 * it is measuring. Everything else here is an increment on a path that was already going
 * to run, and stays compiled in unconditionally.
 *
 * Build with -DAWDL_DIAG_DETAIL=1 to install them for a session. With it off the fields
 * stay zero and every consumer still compiles unchanged: a counter that is never
 * incremented reads 0, which is exactly what a status line should print for an instrument
 * that is not installed. The fields it governs are marked DETAIL below; the producers are
 * in awdl_port_esp32.cpp, each with the cost that put it behind this switch.
 */
#ifndef AWDL_DIAG_DETAIL
#define AWDL_DIAG_DETAIL 0
#endif

struct AwdlDiag {
  /* --- the receive path ----------------------------------------------------------- */
  volatile uint32_t act;      /* AWDL action frames seen */
  volatile uint32_t cb_count; /* sniffer callback entries */
  volatile uint32_t cb_max_us, cb_sum_us;
  volatile uint32_t proc_max_us; /* worst process_frame; this and cb_max_us are
                                    "worst since the last read", and the reader is
                                    what resets them -- awdl_diag_read(clear_maxima) */

  /* --- the estimator -------------------------------------------------------------- */
  volatile uint32_t sync_sw; /* elected-master switches */
  /* DETAIL. The accumulation is two double adds, a multiply and a division on the frame
     path, inside the critical section handle_sync already holds -- and the answer it
     builds, the innovation's mean and spread, is a question you ask while tuning the
     estimator, not one a running transfer depends on. */
  uint32_t sub_n;        /* innovations accumulated */
  double sub_s1, sub_s2; /* sum and sum of squares (us) */
  uint32_t ehist[24];    /* innovation histogram, -24..+24 ms in 2 ms bins */
  volatile uint32_t gauge_skip;

  /* --- the election --------------------------------------------------------------- */
  volatile uint32_t elect_fb, elect_fb_ms;   /* fallback episodes and their dwell */
  volatile uint32_t self_cand, self_cand_ms; /* a peer advertised US as master */

  /* --- the transmit path ---------------------------------------------------------- */
  volatile uint32_t tx_count;
  volatile int32_t tx_err; /* last esp_err from ANY transmit */
  volatile uint32_t mif_tx, mif_tx_err, mif_noidx;
  volatile int32_t mif_last_err; /* ...and the MIF's OWN last result, unclobbered */
  volatile uint16_t mif_last_n;
  /* DETAIL, both. Filling them takes a window snapshot, a PLL extrapolation and two
     64-bit modulos BETWEEN build_mif() and esp_wifi_80211_tx() -- i.e. the measurement
     of where the frame lands is itself part of what moves it. */
  volatile uint32_t txslot_hist[16]; /* which AWC slot our frames actually landed in */
  volatile uint32_t txinto_hist[8];  /* ...and how far into the ch6 slot */

  /* --- the netif ------------------------------------------------------------------ */
  volatile uint32_t netif_tx, netif_tx_sent, netif_txq_drop;
  /* sent counts what the RADIO ACCEPTED, not what we handed it. The difference is not
     pedantry: this driver refuses frames silently, so a transmit path that is refused on
     every single attempt looks identical to one that works until somebody reads the
     return value. */
  volatile uint32_t netif_tx_err;
  volatile int32_t netif_tx_last_err;
  /* WHAT was refused, not just how many. esp_wifi_80211_tx enforces TWO independent rules
     and a refusal by either one arrives as the same ESP_ERR_INVALID_ARG, with nothing
     printed, so the single netif_tx_err counter cannot tell them apart:

       LENGTH. It accepts a raw frame of 24..1500 bytes and refuses anything longer
       (ieee80211_raw_frame_sanity_check; the range esp_wifi.h:1191 documents). len is the
       field that names this one: refusals that are unicast, nh 6 and len 1536 are over the
       ceiling, not misaddressed. Our own headroom is 40 bytes, so the IPv6 packet ceiling
       is 1460 -- see AWDL_NETIF_MTU.

       ADDR1. It refuses a DATA frame whose addr1 is a group address and lets a unicast one
       through; ACTION frames are exempt. Splitting the
       refusals by addr1 says immediately whether a stalled upload lost ITS OWN data frames
       or lost the IPv6 neighbour-discovery traffic underneath them.

     nh/icmp name the refused packet: nh 6 = TCP, 58 = ICMPv6, and then icmp 135 =
     Neighbour Solicitation, 143 = MLDv2. */
  /* The MTU cap's OWN result. It cannot share g_diag.tx_err: that latch is last-write-wins
     across every transmitter and send_mif overwrites it four times a second, so a failure
     recorded there is gone within 250 ms. The whole send path depends on this one call
     succeeding, so it gets a field nobody else touches.
     0 = not attempted yet, ESP_OK = the cap is in place. */
  volatile int32_t netif_mtu_err;
  volatile uint16_t netif_mtu_set; /* the value actually read back from the netif */
  /* Frames rejected by our own ceiling before the driver ever sees them. Counted, not
     returned as ESP_OK: an uncounted rejection is an invisible drop, in a file that
     promises the opposite. */
  volatile uint32_t netif_tx_oversize;
  volatile uint32_t netif_tx_err_mcast, netif_tx_err_ucast;
  volatile uint8_t netif_tx_err_a1[6];
  volatile uint16_t netif_tx_err_len;
  volatile uint8_t netif_tx_err_nh, netif_tx_err_icmp;
  volatile uint16_t txq_depth_max;

  /* Completion reported by the Wi-Fi driver AFTER raw-frame injection. The return from
     esp_wifi_80211_tx only says the driver accepted the frame; these counters separate
     that from its later on-air TX result. Data is split by addr1 because a unicast
     failure during an upload and an expected multicast failure have very different
     consequences. Written in the Wi-Fi task's TX callback; diagnostic only. */
  volatile uint32_t txdone_ucast_ok, txdone_ucast_fail;
  volatile uint32_t txdone_mcast_ok, txdone_mcast_fail;
  volatile uint32_t txdone_action_ok, txdone_action_fail;
  volatile uint32_t netif_retry_queued, netif_retry_drop, netif_retry_exhausted;
  volatile uint16_t netif_inflight, netif_inflight_max;

  /* --- phase histograms ----------------------------------------------------------- */
  volatile uint32_t hist[16];
  float srchist[16];
  uint32_t srchist_n;

  /* --- frames, by what they turned out to be -------------------------------------- */
  volatile uint32_t data, sync_frames, data_any, data_ucast, to_us;
  volatile uint32_t v6_icmp, v6_ns, v6_mdns, v6_other, v6_svc, v6_adrp;
  volatile uint32_t ns_to_us; /* NS whose TARGET is our link-local: a peer is
                                 resolving US, so it means to connect */
  volatile uint32_t netif_rx; /* frames pushed up into lwIP */
  /* Follows AWDL_DIAG_PAYLOAD_MAX, not AWDL_DIAG_DETAIL: this counts CAPTURES, and with
     payload capture off there is nothing to capture -- the count of the frames themselves
     is v6_svc, above, which is always kept. */
  volatile uint32_t svc_dump; /* AWDL service TLVs captured */

  /* --- published window state, counted per second BY THE LIBRARY -------------------
   * Counted where the state is published, never where it is printed: counted in a caller's
   * status line, a user who never prints one gets zeros from an accessor that looks like
   * it is measuring something. */
  volatile uint32_t win_coast_s, win_void_s;

  /* --- folded in from the objects that own them, in awdl_diag_read() ---------------- */
  volatile uint32_t ring_drop;           /* frames the RX ring had nowhere to put */
  volatile uint32_t dump_drop;           /* staged captures dropped, ring full */
  volatile uint32_t pll_relock, pll_out; /* hard re-seeds; rejected innovations */

  /* --- the first link-local we saw a peer use (a connection is about to be tried) --- */
  uint8_t peer_ll[16];
  volatile bool peer_ll_valid;

  volatile int8_t last_rssi;
};
