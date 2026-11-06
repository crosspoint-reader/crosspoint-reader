// AWDL availability-window gauge: the mesh AW-cycle boundary in OUR clock.
//
// Deliberately dependency-free integer C. The SAME header is compiled into the
// firmware and into a host test binary (tools/gauge_test.c) that replays a recorded
// PSF trace and scores it, so the algorithm is testable on a laptop. That is not a
// nicety: the previous design shipped a defect that only a human noticing the badge
// had vanished from the AirDrop UI could detect.
//
// ---------------------------------------------------------------------------
// THE ALGEBRA THIS IS BUILT ON
// ---------------------------------------------------------------------------
// A received PSF frame gives our-clock receive time `rx`, the sender's own TSF at
// the instant the frame hit the air `ptx`, and the mesh-wide AW sequence `aws`.
// Writing k = aws mod 64 and B for the sender's AW-grid origin inside its own TSF,
// the gauge (the mesh AWC boundary in our clock) is
//     phase = (B + rx - ptx) mod AWC
// and B is constrained by  (ptx - B) mod AWC in [k*AW, (k+1)*AW).
// Substituting u = (ptx - B) mod AWC gives phase = (rx - u) mod AWC, hence
//
//     phase lies in (h - AW, h]   where   h = (rx - k*AW) mod AWC
//
// **h contains neither ptx nor B.** Every frame from every sender therefore
// constrains the ONE mesh gauge, using only (rx, aws). There are not N per-sender
// estimation problems; there is one problem with thousands of constraints.
//
// This matters because the old code estimated B per sender from that sender's own
// history. Measured on a 5159-frame trace: two of four senders had NO single valid B
// (their ptx takes ~400ms TSF re-base steps, so their evidence split into clusters
// 8-19 AW apart), and the estimator latched whichever cluster its 16-sample ring
// happened to hold -- confidently, self-consistently, permanently, and 8 AW wrong.
// Because ptx cancels out of h, those same two senders satisfy the h-interval as
// well as the good pair (71.9% and 73.3% versus 75.6% and 78.1%) and are useful
// rather than poisonous. B is now DERIVED from the consensus gauge, never estimated.
//
// Roles are split so no single mechanism owns everything:
//   * the histogram of h-intervals  -> acquisition and the fine plateau position
//   * one-sender-one-vote suffrage  -> the AW integer, exclusively (a frame flooder
//                                      can outvote a frame-weighted histogram)
//   * (rx - ptx) per sender         -> sub-100us precision within that AW
//   * a 32-frame health register     -> falsifies the gauge continuously
#pragma once
#include <stdint.h>
#include <string.h>

#define GA_TU 1024u
#define GA_AW (16u * GA_TU)  /* 16384 */
#define GA_AWC (64u * GA_AW) /* 1048576 == 2^20 */
#define GA_MASK (GA_AWC - 1u)
#define GA_BIN 1024u /* = 1 TU; AWC/BIN = 1024 = 2^10 */
#define GA_NB 1024
#define GA_BSH 10  /* v >> GA_BSH == v / GA_BIN */
#define GA_NV 16   /* AW/BIN: box-car width of one vote */
#define GA_AWSH 14 /* signed e >> GA_AWSH == floor(e/AW) */

#define GA_SRC 12           /* sender slots */
#define GA_NONE 0xFFFFFFFFu /* "no phase sample this frame" */

/* --- constants, each derived from captures rather than picked ------------------ */
#define GA_MINPEAK 24
#define GA_DOM 2
#define GA_EVAL_US 250000u
#define GA_DECAY_US 16000000u
#define GA_VOTE_MIN_US 60000u
#define GA_CONF_US 30000000u
#define GA_ANCHOR_US 8000000u
#define GA_ANCH_SH 3
#define GA_STEP_ACQ 2048
#define GA_STEP_TRK 256
#define GA_MARCH_CLAMP ((int32_t)(GA_AW / 4))
#define GA_JUMP_MIN ((int32_t)(GA_AW / 4))
#define GA_FIX_COOL_US 2000000u
#define GA_TOL ((int32_t)(GA_AW / 2))
#define GA_FAILS 2
#define GA_INNOV_CLAMP ((int32_t)(GA_AW / 8))
#define GA_RATE_CAP_PPB 300000
#define GA_ACTIVE_US 10000000u
#define GA_QUAR_N 8
#define GA_QUAR_WIN_US 30000000u
#define GA_QUAR_US 30000000u
#define GA_HEALTH_W 32
#define GA_HEALTH_MIN 8
#define GA_HEALTH_N 3
#define GA_HEALTH_DEAD 40
#define GA_AWV_MIN 4
#define GA_AWV_SAT 24
#define GA_AWV_STALE_US 8000000u
#define GA_SUFF_MIN 2

typedef struct {
  uint8_t used, b_ok, fail, rp;
  uint8_t mac[6];
  int8_t av_n;  /* Boyer-Moore candidate AW offset */
  uint8_t av_c; /* its count */
  uint32_t B, vt, rx_last, rp_t, av_t, qu;
  uint8_t qu_valid, clkoff_valid;
  uint32_t clkoff_prev; /* (rx - ptx); a big step means the TSF re-based */
  uint32_t rebase;      /* logging only */
  int32_t rebase_last;
} ga_src_t;

typedef struct {
  uint16_t hist[GA_NB];
  ga_src_t src[GA_SRC];
  uint32_t t0, t_dec, t_eval, t_conf, t_lock, t_fix, c_t;
  uint8_t started, locked, t_conf_valid, t_fix_valid, s_prev_valid, disc;
  uint32_t c_off;
  int32_t c_rate; /* ppb of our clock */
  int32_t march, s_prev;
  uint32_t hbits;
  uint8_t hn, unhealthy;
  /* counters for the STAT line */
  uint32_t n_reacq, n_awfix, n_alarm, n_quar, n_sick, n_veto, n_suff;
  uint8_t n_op, n_active;
} ga_t;

/* --- helpers ---------------------------------------------------------------- */
static inline int32_t ga_wrap(int32_t x) { /* to (-AWC/2, AWC/2] */
  uint32_t u = ((uint32_t)x) & GA_MASK;
  return (u > GA_AWC / 2u) ? (int32_t)u - (int32_t)GA_AWC : (int32_t)u;
}
static inline int32_t ga_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
/* Histogram indexing MUST normalise into uint32 before shifting: the correction
   paths legitimately pass negative offsets, and `off / BIN % NB` on a signed int
   truncates toward zero and yields a negative index. */
static inline int ga_bin(int32_t off) { return (int)((((uint32_t)off) & GA_MASK) >> GA_BSH); }

#define GA_REBASE_US                                            \
  100000 /* a step this big in (rx - ptx) is a TSF re-base, not \
            drift: worst-case normal is 200ppm over a 15s gap   \
            ~ 3ms, and a real re-base measured ~4e8 us. */

static void ga_reset(ga_t* g) { memset(g, 0, sizeof(*g)); }

static ga_src_t* ga_row(ga_t* g, const uint8_t* mac, uint32_t rx) {
  int free_i = -1, stalest = 0;
  uint32_t oldest = 0xffffffffu;
  for (int i = 0; i < GA_SRC; i++) {
    if (g->src[i].used) {
      if (!memcmp(g->src[i].mac, mac, 6)) return &g->src[i];
      uint32_t age = rx - g->src[i].rx_last;
      if (age > oldest || oldest == 0xffffffffu) {
        oldest = age;
        stalest = i;
      }
    } else if (free_i < 0)
      free_i = i;
  }
  ga_src_t* s = &g->src[free_i >= 0 ? free_i : stalest];
  memset(s, 0, sizeof(*s));
  s->used = 1;
  memcpy(s->mac, mac, 6);
  s->rx_last = rx;
  return s;
}

static inline uint32_t ga_cpred(const ga_t* g, uint32_t t) {
  int64_t d = (int64_t)g->c_rate * (int32_t)(t - g->c_t) / 1000000000;
  return (uint32_t)(((uint32_t)((int32_t)g->c_off + (int32_t)d)) & GA_MASK);
}

/* Move the gauge AND every derived B by the same amount: phase-minus-C is then
   invariant, so correcting the gauge never makes a healthy sender look broken. */
static void ga_shift(ga_t* g, int32_t d) {
  g->c_off = ((uint32_t)((int32_t)g->c_off + d)) & GA_MASK;
  for (int i = 0; i < GA_SRC; i++)
    if (g->src[i].used && g->src[i].b_ok) g->src[i].B = ((uint32_t)((int32_t)g->src[i].B + d)) & GA_MASK;
}
static void ga_reverse(uint16_t* hist, int first, int end) {
  while (first < --end) {
    uint16_t value = hist[first];
    hist[first++] = hist[end];
    hist[end] = value;
  }
}
static void ga_rotate(ga_t* g, int32_t d) {
  int shift = ga_bin(d);
  if (!shift) return;
  ga_reverse(g->hist, 0, GA_NB);
  ga_reverse(g->hist, 0, shift);
  ga_reverse(g->hist, shift, GA_NB);
}

/* --- one-sender-one-vote on the AW integer ----------------------------------
   The AW integer is the ONLY decision that can produce a >= 1 AW error, and the
   histogram is weighted per FRAME, so a chatty peer can outvote the honest
   population (measured: a coherent 100Hz liar captured the frame-weighted gauge
   completely, 4533 wrong samples; with suffrage, 64, all inside the first 1.4s).
   Sender-weighted voting also PROPOSES the fix a flooder has blocked. */
static int ga_sender_vote(ga_t* g, uint32_t rx, int* agree_out, int* opining_out) {
  int8_t cand[GA_SRC];
  uint8_t cnt[GA_SRC];
  int nc = 0, opining = 0, active = 0;
  for (int i = 0; i < GA_SRC; i++) {
    ga_src_t* s = &g->src[i];
    if (!s->used) continue;
    if ((uint32_t)(rx - s->rx_last) >= GA_ACTIVE_US) continue;
    active++;
    if (s->av_c < GA_AWV_MIN) continue;
    if ((uint32_t)(rx - s->av_t) > GA_AWV_STALE_US) continue;
    opining++;
    int f = -1;
    for (int j = 0; j < nc; j++)
      if (cand[j] == s->av_n) {
        f = j;
        break;
      }
    if (f < 0) {
      cand[nc] = s->av_n;
      cnt[nc] = 1;
      nc++;
    } else
      cnt[f]++;
  }
  g->n_op = (uint8_t)opining;
  g->n_active = (uint8_t)active;
  int bi = -1, bc = 0;
  for (int j = 0; j < nc; j++)
    if (cnt[j] > bc) {
      bc = cnt[j];
      bi = j;
    }
  if (agree_out) *agree_out = bc;
  if (opining_out) *opining_out = opining;
  if (bi < 0 || bc < GA_SUFF_MIN || bc * 2 <= opining) return 0;
  return cand[bi];
}

/* --- periodic evaluation (call from a task, NOT from a critical section) ----- */
static void ga_eval(ga_t* g, uint32_t rx) {
  if (g->locked && g->hn >= GA_HEALTH_W) {
    int pc = 0;
    for (uint32_t b = g->hbits; b; b >>= 1) pc += (int)(b & 1u);
    if (pc < GA_HEALTH_MIN) {
      g->unhealthy++;
      if (g->unhealthy == GA_HEALTH_N) {
        memset(g->hist, 0, sizeof(g->hist));
        g->disc = 1;
        g->n_sick++;
      } else if (g->unhealthy >= GA_HEALTH_DEAD) {
        uint32_t keep_reacq = g->n_reacq + 1;
        uint32_t k2 = g->n_awfix, k3 = g->n_alarm, k4 = g->n_quar, k5 = g->n_sick, k6 = g->n_veto, k7 = g->n_suff;
        ga_src_t keep[GA_SRC];
        memcpy(keep, g->src, sizeof(keep));
        ga_reset(g);
        memcpy(g->src, keep, sizeof(keep));
        for (int i = 0; i < GA_SRC; i++) {
          g->src[i].b_ok = 0;
          g->src[i].av_c = 0;
        }
        g->n_reacq = keep_reacq;
        g->n_awfix = k2;
        g->n_alarm = k3;
        g->n_quar = k4;
        g->n_sick = k5;
        g->n_veto = k6;
        g->n_suff = k7;
        g->started = 1;
        g->t0 = g->t_dec = g->t_eval = g->c_t = rx;
        return;
      }
    } else
      g->unhealthy = 0;
  }

  int pos = 0;
  uint16_t peak = 0;
  for (int i = 0; i < GA_NB; i++)
    if (g->hist[i] > peak) {
      peak = g->hist[i];
      pos = i;
    }
  uint16_t comp = 0;
  for (int s = -1; s <= 1; s += 2) {
    int i = (pos + s * GA_NV) & (GA_NB - 1);
    if (g->hist[i] > comp) comp = g->hist[i];
  }
  int strong = (peak >= GA_MINPEAK) && (peak >= (uint16_t)(GA_DOM * comp));
  if (strong) {
    g->t_conf = rx;
    g->t_conf_valid = 1;
  }

  if (!g->locked) {
    if (strong) {
      g->c_off = ((uint32_t)pos * GA_BIN) & GA_MASK;
      g->c_t = rx;
      g->c_rate = 0;
      g->t_lock = rx;
      g->locked = 1;
      g->march = 0;
      memset(g->hist, 0, sizeof(g->hist));
      g->s_prev = 0;
      g->s_prev_valid = 0;
      g->disc = 0;
    }
    return;
  }
  if (!strong) return;

  /* while locked the histogram is kept in C-relative coordinates, so the peak IS
     the correction the mesh is asking for */
  int32_t sv = ga_wrap((int32_t)((uint32_t)pos * GA_BIN));

  int32_t d = g->s_prev_valid ? ga_wrap(sv - g->s_prev) : 0;
  if (d >= GA_JUMP_MIN || d <= -GA_JUMP_MIN) {
    if (!g->disc) memset(g->hist, 0, sizeof(g->hist));
    g->disc = 1;
  }

  int agree = 0, opining = 0;
  int naw = ga_sender_vote(g, rx, &agree, &opining);
  int cool_ok = (!g->t_fix_valid) || ((uint32_t)(rx - g->t_fix) >= GA_FIX_COOL_US);
  if (naw != 0 && cool_ok) {
    int32_t mv = (int32_t)naw * (int32_t)GA_AW;
    ga_shift(g, mv);
    ga_rotate(g, mv);
    g->t_fix = rx;
    g->t_fix_valid = 1;
    g->n_awfix++;
    g->n_suff++;
    g->march = 0;
    g->s_prev_valid = 0;
    g->disc = 0;
    for (int i = 0; i < GA_SRC; i++) g->src[i].av_c = 0;
    return;
  }
  /* a whole-AW proposal that suffrage did not ratify is a veto, worth seeing */
  if ((sv >= GA_TOL || sv <= -GA_TOL) && naw == 0) g->n_veto++;

  if (sv < GA_JUMP_MIN && sv > -GA_JUMP_MIN) g->disc = 0;
  int acq = ((uint32_t)(rx - g->t_lock) < GA_ANCHOR_US);
  int32_t lim = acq ? GA_STEP_ACQ : GA_STEP_TRK;
  int32_t step = ga_clamp(sv >> GA_ANCH_SH, -lim, lim);
  int32_t applied;
  if (acq) {
    ga_shift(g, step);
    applied = step;
  } else {
    int32_t na = ga_clamp(g->march + step, -GA_MARCH_CLAMP, GA_MARCH_CLAMP);
    applied = na - g->march;
    ga_shift(g, applied);
    g->march = na;
    if (g->march >= GA_MARCH_CLAMP || g->march <= -GA_MARCH_CLAMP) g->n_alarm++;
  }
  g->s_prev = ga_wrap(sv - applied);
  g->s_prev_valid = 1;
}

/* --- per-frame entry point. Returns a phase sample for the window PLL, or
       GA_NONE. Call from the frame-processing task, outside any critical section
       (it mutates the sender table and the gauge, which the log path only reads). */
static uint32_t ga_on_frame(ga_t* g, const uint8_t* mac, uint32_t rx, uint32_t ptx, uint16_t aws) {
  uint32_t k = (uint32_t)(aws & 63u);
  uint32_t h = (rx - k * GA_AW) & GA_MASK; /* the whole constraint, one line */
  ga_src_t* s = ga_row(g, mac, rx);
  s->rx_last = rx;

  /* Re-base fast path: drop this sender's DERIVED B at once rather than waiting for
     the disagreement test below to notice. Note this is not a reason to exclude the
     sender -- replayed alone, the trace's two chronic re-basers feed 410 and 1486
     phase samples with zero errors, because the gauge itself never depends on ptx. */
  uint32_t clkoff = rx - ptx;
  if (s->clkoff_valid) {
    int32_t d = (int32_t)(clkoff - s->clkoff_prev);
    if (d > (int32_t)GA_REBASE_US || d < -(int32_t)GA_REBASE_US) {
      s->rebase++;
      s->rebase_last = d;
      s->b_ok = 0;
      s->fail = 0;
    }
  }
  s->clkoff_prev = clkoff;
  s->clkoff_valid = 1;
  if (!g->started) {
    g->started = 1;
    g->t0 = g->t_dec = g->t_eval = g->c_t = rx;
  }

  if (g->locked) {
    int32_t e = ga_wrap((int32_t)h - (int32_t)ga_cpred(g, rx));
    int8_t n = (int8_t)(e >> GA_AWSH); /* arithmetic shift = floor div */
    if (s->av_c == 0) {
      s->av_n = n;
      s->av_c = 1;
    } else if (s->av_n == n) {
      if (s->av_c < GA_AWV_SAT) s->av_c++;
    } else
      s->av_c--;
    s->av_t = rx;
  }

  if (s->vt == 0 || (uint32_t)(rx - s->vt) >= GA_VOTE_MIN_US) {
    s->vt = rx | 1u;
    int32_t v = g->locked ? (int32_t)((h - ga_cpred(g, rx)) & GA_MASK) : (int32_t)h;
    int b = ga_bin(v);
    for (int j = 0; j < GA_NV; j++) {
      int i = (b - j) & (GA_NB - 1);
      if (g->hist[i] < 60000) g->hist[i]++;
    }
  }

  if (g->locked) {
    int32_t dh = ga_wrap((int32_t)h - (int32_t)ga_cpred(g, rx));
    int hit = (dh >= 0 && dh < (int32_t)GA_AW) ? 1 : 0;
    g->hbits = (g->hbits << 1) | (uint32_t)hit;
    if (g->hn < GA_HEALTH_W) g->hn++;
  }

  if ((uint32_t)(rx - g->t_dec) >= GA_DECAY_US) {
    g->t_dec = rx;
    for (int i = 0; i < GA_NB; i++) g->hist[i] >>= 1;
  }
  if ((uint32_t)(rx - g->t_eval) >= GA_EVAL_US) {
    g->t_eval = rx;
    ga_eval(g, rx);
  }

  if (!g->locked) return GA_NONE;
  if (!g->t_conf_valid || (uint32_t)(rx - g->t_conf) > GA_CONF_US) return GA_NONE;
  if (s->qu_valid && (uint32_t)(rx - s->qu) < GA_QUAR_US) return GA_NONE;

  uint32_t Cp = ga_cpred(g, rx);
  uint32_t q = (rx - ptx) & GA_MASK;
  if (!s->b_ok) {
    s->B = (Cp - q) & GA_MASK;
    s->b_ok = 1;
    s->fail = 0;
  }
  uint32_t ph = (q + s->B) & GA_MASK;
  int32_t e = ga_wrap((int32_t)ph - (int32_t)Cp);
  if (e >= GA_TOL || e <= -GA_TOL) {
    if (++s->fail >= GA_FAILS) {
      if ((uint32_t)(rx - s->rp_t) > GA_QUAR_WIN_US) {
        s->rp = 0;
        s->rp_t = rx;
      }
      if (++s->rp >= GA_QUAR_N) {
        s->qu = rx;
        s->qu_valid = 1;
        s->rp = 0;
        g->n_quar++;
      }
      s->B = (Cp - q) & GA_MASK;
      s->fail = 0; /* its TSF stepped: re-pin */
    }
    return GA_NONE;
  }
  s->fail = 0;

  uint32_t dt = rx - g->c_t;
  if (dt) {
    int64_t adv = (int64_t)g->c_rate * (int32_t)dt / 1000000000;
    g->c_off = ((uint32_t)((int32_t)g->c_off + (int32_t)adv)) & GA_MASK;
    g->c_t = rx;
    int32_t e2 = ga_clamp(ga_wrap((int32_t)ph - (int32_t)g->c_off), -GA_INNOV_CLAMP, GA_INNOV_CLAMP);
    int32_t w = (int32_t)(dt > 1000000u ? 1000000u : dt);
    g->c_off = ((uint32_t)((int32_t)g->c_off + (int32_t)((int64_t)e2 * w / 4000000))) & GA_MASK;
    g->c_rate = ga_clamp(g->c_rate + (int32_t)((int64_t)e2 * w / 30000), -GA_RATE_CAP_PPB, GA_RATE_CAP_PPB);
  }
  return ph;
}

/* --- accessors for the firmware's logging / election ------------------------ */
static uint32_t ga_rebase_of(const ga_t* g, const uint8_t* mac) {
  for (int i = 0; i < GA_SRC; i++)
    if (g->src[i].used && !memcmp(g->src[i].mac, mac, 6)) return g->src[i].rebase;
  return 0;
}
static uint32_t ga_rebase_total(const ga_t* g) {
  uint32_t t = 0;
  for (int i = 0; i < GA_SRC; i++)
    if (g->src[i].used) t += g->src[i].rebase;
  return t;
}
static int ga_health(const ga_t* g) { /* 0..32; the audit */
  int pc = 0;
  for (uint32_t b = g->hbits; b; b >>= 1) pc += (int)(b & 1u);
  return pc;
}
