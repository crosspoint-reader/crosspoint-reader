// Master election: given what we have heard from each candidate, which one do we
// sync to? Extracted from select_master() as a pure function so it can be tested on
// the host, because this is the most dangerous code in the project.
//
// Its history is the reason for that judgement. Electing by METRIC tracked whichever
// peer momentarily claimed the biggest number and flapped 842 times in four minutes,
// and the badge left the Mac's AWDL peer table exactly when we began advertising that
// peer. Latching a decision "because it is a constant" left the gauge 131ms out and
// the badge invisible for hours. Both were found by a human noticing the badge had
// vanished -- not by a test. This header exists so the next change to the rules can
// be argued about against fixtures instead.
//
// What is IN here: eligibility (staleness, sample count, self, trust) and the choice
// (lowest inter-announcement gap, metric as tie-break, stdev as second tie-break,
// hysteresis for the incumbent). What is deliberately OUT: the phase statistics, the
// global PLL, the channel mask, and everything else select_master() does with the
// winner -- those need the firmware's clocks and rings, and none of them changes
// which candidate wins.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// How long a candidate may stay silent and still be eligible. The incumbent gets
// three times as long: it drifts between meshes otherwise -- measured, ce57 (metric
// 517, ch6 slot 8, the Mac's own master) going quiet for >5s dropped us onto a peer
// on slot 0, which moved our TX window off the Mac's ch6 window and killed
// reachability. The global PLL keeps the window phase continuous meanwhile.
#define EL_STALE_CUR_MS 15000
#define EL_STALE_OTHER_MS 5000
// Below this many phase samples a row is not evidence of anything yet.
#define EL_MIN_SAMPLES 4

struct ElectCand {
  bool used;
  uint8_t addr[6];
  uint32_t n;        // phase samples collected from this sender
  uint32_t metric;   // its advertised top_master_metric
  uint32_t last_ms;  // when we last heard it
  uint32_t gap_avg;  // mean ms between its announcements; 0xffffffff = unknown
  float stdev;       // phase stdev, second tie-break only
  bool untrusted;    // caller's src_untrusted(): a chronic TSF re-baser
};

struct ElectResult {
  int chosen;      // index into the candidate array, or -1 for nobody
  int count;       // eligible candidates, i.e. STAT's masters=
  bool fallback;   // pass 0 found nobody, so re-basers were admitted
  bool self_seen;  // a row keyed on our own address was refused a vote
};

// Returns true when `a` beats the incumbent-agnostic best so far.
static inline bool el_better(uint32_t gap, uint32_t metric, float stdev, int best, uint32_t best_gap,
                             uint32_t best_metric, float best_std) {
  if (best < 0) return true;
  if (gap != best_gap) return gap < best_gap;
  if (metric != best_metric) return metric > best_metric;
  return stdev < best_std;
}

static struct ElectResult elect_choose(const struct ElectCand* c, int n_cand, uint32_t now, const uint8_t* own_mac,
                                       const uint8_t* cur_sel, bool have_sel) {
  struct ElectResult r;
  r.chosen = -1;
  r.count = 0;
  r.fallback = false;
  r.self_seen = false;
  int best = -1, sel = -1;
  uint32_t best_metric = 0, best_gap = 0xffffffff, sel_gap = 0xffffffff;
  float best_std = 1e30f;

  // Pass 0 admits only trusted senders. Pass 1 runs ONLY if pass 0 found nobody at
  // all, and admits the chronic re-basers too.
  //
  // The fallback is there because the ban is permanent: it reads a re-base counter
  // that only ever increments, so a phone that re-bases early is excluded for the
  // rest of the power cycle. When the mesh re-roots onto that phone every other
  // device follows it, the Mac included -- and only we could not, so we waited for a
  // master nobody was advertising any more. Measured: 74 seconds with no master, no
  // MIF sent for the whole of it, and macOS aged us out of its neighbour table after
  // ~6-8s of silence. Following a re-baser is the lesser evil: its window was
  // measured identical to everyone else's in all 17 observed gaps, and the
  // alternative is not a safer window but no window at all.
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < n_cand; i++) {
      bool is_cur = (have_sel && memcmp(c[i].addr, cur_sel, 6) == 0);
      uint32_t stale_lim = is_cur ? EL_STALE_CUR_MS : EL_STALE_OTHER_MS;
      if (!c[i].used || c[i].n < EL_MIN_SAMPLES || (now - c[i].last_ms) > stale_lim) continue;
      // Never elect ourselves. Such a row exists because a foreign peer named us in
      // its sync-params master field; electing it makes the gauge measure our clock
      // against our own, which was measured collapsing gauge health from 30 to 0 for
      // 24 seconds. We advertise ourselves as a leaf with a deliberately weak metric
      // (claiming root made iOS treat us as out-of-mesh and refuse to peer), so there
      // is no state in which electing ourselves is right.
      if (memcmp(c[i].addr, own_mac, 6) == 0) {
        r.self_seen = true;
        continue;
      }
      if (pass == 0 && c[i].untrusted) continue;
      r.count++;
      if (is_cur) {
        sel = i;
        sel_gap = c[i].gap_avg;
      }
      if (el_better(c[i].gap_avg, c[i].metric, c[i].stdev, best, best_gap, best_metric, best_std)) {
        best = i;
        best_metric = c[i].metric;
        best_gap = c[i].gap_avg;
        best_std = c[i].stdev;
      }
    }
    if (best >= 0) break;  // pass 0 sufficed: nothing changes
    if (pass == 0) {
      r.count = 0;
      r.fallback = true;
    }
  }

  // Hysteresis on the gap: hold the incumbent unless a challenger is heard clearly
  // more often (1.5x). Ties and small differences must not move us -- every switch
  // changes the master_addr we advertise, and that is what the Mac judges us on.
  r.chosen = best;
  if (sel >= 0 && (uint64_t)sel_gap * 2 <= (uint64_t)best_gap * 3) r.chosen = sel;
  return r;
}
