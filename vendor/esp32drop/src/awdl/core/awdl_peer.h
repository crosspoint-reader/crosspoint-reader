/* awdl_peer.h -- the mesh-wide window PLL.
 *
 * AWDL synchronises the whole vicinity to ONE availability-window schedule, so every
 * master's sync frame measures the SAME window phase. This tracks that one phase, and
 * everything downstream depends on it: the transmit gate in awdl_window.h asks where in
 * the cycle we are, and if this answer drifts the badge transmits into an empty slot.
 * That has happened -- 8 AW out, 131 ms, 4,125 frames into a window nobody was listening
 * on, found only because a human noticed the badge had left the AirDrop UI.
 *
 * It is a single global loop on purpose. A per-master PLL was tried and reset itself every
 * time the election flapped between equally-synced masters, which dropped the lock -- and
 * therefore the TX timing -- whenever nearby Apple devices woke up and churned the
 * election.
 *
 * The constants are tuned on hardware and each one has a measured failure behind it:
 *   KP 0.06 / KI 2e-10   phase and drift gains; the integral gain is small because the
 *                        drift is slow (measured 0.1-38 ppm against the elected master)
 *   OUTLIER 15,000 us    innovations beyond this are not measurements, they are a
 *                        different mesh or a re-based TSF
 *   8 consecutive misses re-seed. A regime change must be followed, but not instantly:
 *                        one jitter observation once became a permanent latch and left
 *                        the gauge 131 ms out for hours.
 *   WANDER 1,500 us      "locked" means the SMOOTHED track is stable, not that the last
 *                        sample was close
 *
 * Dependency-free apart from <math.h> for fmod/fabs: it compiles unchanged into the
 * firmware and into tools/test-peer.sh, where the sample stream and its timestamps are
 * scripted.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h> /* memcmp/memcpy/memset, used below: a header that calls itself
                         dependency-free has to include what it uses rather than rely on
                         whoever includes it having done so first. */
#include <math.h>

#include "awdl_lru.h"
#include "awdl_window.h"

#define AWDL_PLL_KP 0.06
#define AWDL_PLL_KI 2.0e-10 /* per-us: drift is slow */
#define AWDL_PLL_OUTLIER_US 15000.0
#define AWDL_PLL_FREQ_CLAMP 2e-4
#define AWDL_PLL_RELOCK_MISS 8
#define AWDL_PLL_WANDER_US 1500.0 /* "locked" threshold on the smoothed track */
#define AWDL_PLL_LOCK_MIN_N 20    /* ...and this many accepted samples */

struct AwdlPll {
  double phase;   /* smoothed window phase, wrapped into [0, AWC_US) */
  double freq;    /* drift, us per us of our clock */
  double slow;    /* slow EMA of phase, for wander */
  double wander;  /* EMA of |phase - slow| = smoothed-track jitter */
  double resid;   /* EMA of innovation^2 = raw measurement noise */
  uint32_t tlast; /* rx timestamp of the last update */
  uint32_t n;     /* accepted updates */
  uint32_t miss;  /* consecutive outliers */
  bool init;
  uint32_t n_out;    /* samples the outlier gate rejected */
  uint32_t n_relock; /* hard re-seeds */
};

/* What one sample did. The caller's own statistics -- the innovation histogram, the
   per-sender residuals -- must be updated for every outcome EXCEPT SEEDED, which is the
   one case with no innovation to speak of. */
enum {
  AWDL_PLL_SEEDED = 0, /* first sample: the loop was empty and now holds this */
  AWDL_PLL_ACCEPTED = 1,
  AWDL_PLL_OUTLIER = 2, /* rejected; the loop is unchanged */
  AWDL_PLL_RELOCK = 3   /* rejected, and enough in a row to re-seed on this one */
};

static void awdl_pll_init(struct AwdlPll* p) {
  p->phase = p->freq = p->slow = p->wander = p->resid = 0;
  p->tlast = p->n = p->miss = 0;
  p->init = false;
  p->n_out = p->n_relock = 0;
}

static void awdl_pll_seed(struct AwdlPll* p, int64_t offset, uint32_t rx_ts) {
  p->phase = (double)offset;
  p->freq = 0;
  p->slow = (double)offset;
  p->wander = 0;
  p->resid = 0;
  p->n = 1;
  p->miss = 0;
  p->tlast = rx_ts;
  p->init = true;
}

/* Wrap a difference into (-AWC/2, +AWC/2]. The phase is modular, so an innovation of
   +1,048,000 us is really -576 us and treating it as the former would throw the loop. */
static double awdl_pll_wrap(double e) {
  e = fmod(e, (double)AWC_US);
  if (e > AWC_US / 2)
    e -= AWC_US;
  else if (e < -AWC_US / 2)
    e += AWC_US;
  return e;
}

/* Feed one phase sample. `innovation` receives the residual in microseconds for every
   outcome except SEEDED. */
static int awdl_pll_feed(struct AwdlPll* p, int64_t offset, uint32_t rx_ts, double* innovation) {
  if (!p->init) {
    awdl_pll_seed(p, offset, rx_ts);
    return AWDL_PLL_SEEDED;
  }

  uint32_t dt = rx_ts - p->tlast;
  if (dt == 0) dt = 1;
  double pred = p->phase + p->freq * (double)dt;
  double e = awdl_pll_wrap((double)offset - pred);
  if (innovation) *innovation = e;

  int r;
  if (fabs(e) < AWDL_PLL_OUTLIER_US) {
    p->phase = pred + AWDL_PLL_KP * e;
    p->freq += AWDL_PLL_KI * e;
    if (p->freq > AWDL_PLL_FREQ_CLAMP) p->freq = AWDL_PLL_FREQ_CLAMP;
    if (p->freq < -AWDL_PLL_FREQ_CLAMP) p->freq = -AWDL_PLL_FREQ_CLAMP;
    p->phase = fmod(p->phase, (double)AWC_US);
    if (p->phase < 0) p->phase += AWC_US;
    double dphi = awdl_pll_wrap(p->phase - p->slow);
    p->slow += 0.05 * dphi;
    p->wander = 0.95 * p->wander + 0.05 * fabs(dphi);
    p->resid = 0.95 * p->resid + 0.05 * e * e;
    p->n++;
    p->miss = 0;
    r = AWDL_PLL_ACCEPTED;
  } else {
    p->n_out++;
    if (++p->miss >= AWDL_PLL_RELOCK_MISS) {
      /* Re-seed on THIS sample, not on the next one. A regime change -- the mesh picking
         a different schedule -- has to be followed, and waiting one more frame to do it
         is another frame transmitted into the old window. */
      awdl_pll_seed(p, offset, rx_ts);
      p->n_relock++;
      return AWDL_PLL_RELOCK; /* seed already set tlast */
    }
    r = AWDL_PLL_OUTLIER;
  }
  p->tlast = rx_ts;
  return r;
}

/* Extrapolate the phase to a later rx timestamp. The subtraction is deliberately on
   uint32: AWC_US is 2^20 exactly and 2^32 is a multiple of it, so this is exact across
   the 32-bit wrap of the hardware timestamp. */
static double awdl_pll_predict(const struct AwdlPll* p, uint32_t rx_now) {
  double pred = p->phase + p->freq * (double)(uint32_t)(rx_now - p->tlast);
  pred = fmod(pred, (double)AWC_US);
  if (pred < 0) pred += AWC_US;
  return pred;
}

/* Where in the AW cycle we are, at this rx timestamp: the position the transmit gate
   consumes. The loop tracks an OFFSET between our clock and the mesh schedule, so the
   position is that offset subtracted from the clock, modulo the cycle -- with zero drift
   the offset is constant and the position advances with the clock, which is the part that
   reads backwards until you see it written down.
   All of it on int64 over uint32 differences, exact across the timestamp wrap. */
static int64_t awdl_pll_cycle_phase(const struct AwdlPll* p, uint32_t rx_now) {
  double pred = p->phase + p->freq * (double)(uint32_t)(rx_now - p->tlast);
  int64_t ph = (((int64_t)rx_now - (int64_t)pred) % AWC_US + AWC_US) % AWC_US;
  return ph;
}

/* Locked means the SMOOTHED track is stable and has been fed enough to mean it -- not
   that the last sample happened to land close. */
static bool awdl_pll_locked(const struct AwdlPll* p) {
  return p->init && p->n > AWDL_PLL_LOCK_MIN_N && p->wander < AWDL_PLL_WANDER_US;
}

/* Root-mean-square of the raw measurement noise, in microseconds. The STAT line's
   pll{noise=} -- 12-15 us when healthy, 5,911 us before the estimator was fixed. */
static double awdl_pll_noise_rms(const struct AwdlPll* p) { return sqrt(p->resid); }

/* ---- the master table -------------------------------------------------------
 *
 * One row per AWDL master address heard. It exists for two jobs and no others: the
 * master_addr and master_counter we ECHO in our own MIF, which is how we look like a
 * credible in-mesh peer, and the channel sequence we take the TX window from. Both want
 * the address the mesh AGREES on, which is why the election in awdl_elect.h chooses by
 * consensus (lowest mean inter-announcement gap) rather than by metric -- metric-first
 * flapped 842 times in four minutes and took the badge off the air.
 *
 * Eviction takes the STALEST row, never an active one, so a room full of ambient Apple
 * devices cannot displace the master we are locked to.
 */
#define AWDL_PEERS 8

struct AwdlPeer {
  bool used;
  uint8_t addr[6];
  int64_t off[16]; /* ring of AW-cycle phase estimates (us), ALL samples: the
                      robustness is applied at read time, not at insert time --
                      an insert-time outlier gate once starved the ring */
  uint8_t n, pos;  /* samples filled (<=16), write cursor */
  uint32_t frames;
  uint32_t metric;         /* the root's top_master_metric, as announced */
  uint32_t master_counter; /* the root's master_counter (election-v2). Echoed; frozen at
                              0 reads as out-of-mesh and iOS will not peer */
  bool have_election;
  uint8_t chseq[16];
  bool have_chseq;
  uint32_t last_ms;                 /* millis() of the last frame from this master */
  uint32_t gap_max, gap_sum, gap_n; /* inter-announcement interval: the election's
                                       consensus signal */
  uint16_t last_aw_seq;             /* as announced, raw */
  uint32_t last_aw_ts;              /* ...and the rx timestamp then, to extrapolate it forward */
};

struct AwdlPeerTable {
  struct AwdlPeer row[AWDL_PEERS];
};

static void awdl_peer_table_init(struct AwdlPeerTable* t) {
  for (int i = 0; i < AWDL_PEERS; i++) t->row[i].used = false;
}

/* Find this master's row, or NULL. Separate from awdl_peer_row() because the callers on
   the transmit side -- the ones asking which slots this master's channel sequence puts on
   ch6 -- must not create a row as a side effect of asking. They run inside a critical
   section against the sniffer callback, and allocating there would mean a writer where
   the locking assumes a reader. */
static struct AwdlPeer* awdl_peer_find(struct AwdlPeerTable* t, const uint8_t* addr) {
  for (int i = 0; i < AWDL_PEERS; i++)
    if (t->row[i].used && memcmp(t->row[i].addr, addr, 6) == 0) return &t->row[i];
  return 0;
}

/* Find this master's row, or take one for it. A free slot first, else the stalest.
 *
 * ⚠️ The staleness comparison is awdl_lru_staler(), shared with the two other MAC-keyed
 * tables, and it is wrap-naive: see the warning in awdl_lru.h. tools/test-peer.sh pins
 * both the normal case and the defect.
 */
static struct AwdlPeer* awdl_peer_row(struct AwdlPeerTable* t, const uint8_t* addr) {
  int free_i = -1, stalest = 0;
  uint32_t oldest = 0xffffffffu;
  for (int i = 0; i < AWDL_PEERS; i++) {
    if (t->row[i].used) {
      if (memcmp(t->row[i].addr, addr, 6) == 0) return &t->row[i];
      if (awdl_lru_staler(t->row[i].last_ms, oldest)) {
        oldest = t->row[i].last_ms;
        stalest = i;
      }
    } else if (free_i < 0) {
      free_i = i;
    }
  }
  struct AwdlPeer* m = &t->row[(free_i >= 0) ? free_i : stalest];
  memset(m, 0, sizeof *m);
  m->used = true;
  memcpy(m->addr, addr, 6);
  return m;
}

/* How many rows have been heard from recently. The STAT line's masters=; zero means no
   elected master, which means no TX at all -- a state measured at 74 seconds, during
   which macOS aged the badge out of its peer table entirely. */
static int awdl_peer_live(const struct AwdlPeerTable* t, uint32_t now_ms, uint32_t max_age_ms) {
  int n = 0;
  for (int i = 0; i < AWDL_PEERS; i++)
    if (t->row[i].used && (uint32_t)(now_ms - t->row[i].last_ms) <= max_age_ms) n++;
  return n;
}

/* Robust phase statistics over one peer's recent offsets: a TRIMMED stdev about the
 * MEDIAN, computed on the AWC circle rather than on the line.
 *
 * Moved here from the sketch at extraction step 10. It had to move somewhere: the
 * election (library) and the STAT line (sketch) both call it, so leaving it in either
 * one would have made it a cross-layer call. As a static inline beside the table it
 * summarises, both sides get their own copy and neither pays for a call.
 *
 * Circle, not line, because offsets live modulo AWC_US: two samples 10 us apart across
 * the wrap are 1,048,566 apart on the line. Median-and-trim, not mean-and-sigma, because
 * a re-basing peer contributes a handful of enormous outliers and the mean is exactly the
 * statistic they capture -- which is what the election reads to decide whether a peer is
 * steady enough to follow.
 *
 * n is bounded by the caller's fixed 16-wide offset row; u[] is sized to match.
 */
static inline void phase_stats_raw(const int64_t* off, int n, float* mean, float* stdev) {
  if (n == 0) {
    *mean = 0;
    *stdev = 0;
    return;
  }
  int64_t base = off[0];
  double u[16];
  for (int i = 0; i < n; i++) {
    int64_t d = ((off[i] - base) % AWC_US + AWC_US) % AWC_US;
    if (d > AWC_US / 2) d -= AWC_US;
    u[i] = (double)d;
  }
  for (int i = 1; i < n; i++) {
    double k = u[i];
    int j = i - 1;
    while (j >= 0 && u[j] > k) {
      u[j + 1] = u[j];
      j--;
    }
    u[j + 1] = k;
  }
  double med = (n & 1) ? u[n / 2] : 0.5 * (u[n / 2 - 1] + u[n / 2]);
  const double TRIM = 8000.0;
  double sq = 0;
  int c = 0;
  for (int i = 0; i < n; i++) {
    double e = u[i] - med;
    if (fabs(e) <= TRIM) {
      sq += e * e;
      c++;
    }
  }
  *stdev = c ? (float)sqrt(sq / c) : 0;
  *mean = (float)(base + med);
}
