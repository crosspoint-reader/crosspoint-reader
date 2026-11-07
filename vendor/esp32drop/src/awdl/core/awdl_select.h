/* awdl_select.h -- the outcome of one master election, as the rest of the system sees it.
 *
 * Split out of the repo-root awdl_sync.h at extraction step 10. It lives here rather than
 * in awdl_peer.h because it is not the peer TABLE, it is the ANSWER: one row chosen, with
 * the phase statistics and estimator quality that decided the choice attached to it. The
 * transmit path needs the answer; the diagnostics need the quality; neither needs the
 * table.
 *
 * Dependency-free: plain integers, floats and a MAC. Nothing here includes the window
 * arithmetic or the PLL, so a host test can construct one by hand.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Result of electing/measuring the master we sync to (computed each loop).
struct Selection {
  int idx;            // index into g_peers.row, or -1
  float mean, stdev;  // PLL phase predicted to now (us); raw sample stdev (us)
  uint32_t metric;
  int count;  // number of active masters
  uint8_t addr[6];
  float wander;     // smoothed-track jitter (us) — the lock quality
  float resid_rms;  // raw measurement-noise RMS (us)
  float drift_ppm;  // estimated clock drift vs master (ppm)
  uint32_t pll_n;
  uint16_t cur_aw_seq;      // current AW sequence number, extrapolated to now
  uint32_t master_counter;  // chosen master's root master_counter, to echo in our election TLVs
  bool have_election;       // whether we have real election params for the chosen master
};
