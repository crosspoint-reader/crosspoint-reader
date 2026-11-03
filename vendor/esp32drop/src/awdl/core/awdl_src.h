/* awdl_src.h -- per-SENDER residual statistics.
 *
 * Keyed by the transmitting MAC (pl[10..15]), NOT by the announced sync master the way
 * awdl_peer.h is. The two keys are genuinely different and the difference was measured:
 * every sender announces the same master, but each carries its own TSF epoch -- three
 * senders sat at -180 ms / +14 ms / +275 ms of each other, and EHIST showed exactly three
 * spikes, each under 2 ms wide. A table keyed by the announced master cannot separate
 * them, which is how 977 samples came to be rejected as outliers and the lock duty sat at
 * 83%.
 *
 * What it holds now is diagnostic: the innovation mean and spread per sender (the ESRC
 * line) and each sender's phase histogram at one-AW resolution over the 64-AW cycle (the
 * SHIST line). The histogram settled a real question -- whether the channel sequence
 * advances one AW per step or four -- by showing every sender peaking in the same single
 * place rather than every 16 bins. The answer is 64 AW.
 *
 * Dependency-free C over plain buffers so the same code compiles into both firmwares and
 * into tools/test-src.sh.
 */
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "awdl_lru.h"

/* Twelve rows. Measured occupancy in an ordinary room is three to five senders; twelve
   leaves room for a crowd without the eviction rule mattering. */
#define AWDL_SRC_ROWS 12
#define AWDL_SRC_BINS 64     /* one bin per AW over the 64-AW cycle */
#define AWDL_SRC_CAP 250     /* per-bin saturation, below UINT8_MAX so += cannot wrap */
#define AWDL_SRC_DECAY 0x1ff /* halve every 512 frames, so the shape tracks the present */

struct AwdlSrc {
  bool used;
  uint8_t mac[6];
  uint32_t n;    /* innovations accumulated */
  double s1, s2; /* sum and sum of squares of the innovation (us) */
  uint32_t last_ms;
  uint8_t hist[AWDL_SRC_BINS];
  uint32_t hist_n;
};

struct AwdlSrcTable {
  struct AwdlSrc row[AWDL_SRC_ROWS];
};

static void awdl_src_table_init(struct AwdlSrcTable* t) {
  for (int i = 0; i < AWDL_SRC_ROWS; i++) t->row[i].used = false;
}

/* This sender's row, or a new one for it: a free slot first, else the stalest.
   The staleness comparison is shared -- and wrap-naive; see awdl_lru.h. */
static struct AwdlSrc* awdl_src_row(struct AwdlSrcTable* t, const uint8_t* mac, uint32_t now_ms) {
  int free_i = -1, stalest = 0;
  uint32_t oldest = 0xffffffffu;
  for (int i = 0; i < AWDL_SRC_ROWS; i++) {
    if (t->row[i].used) {
      if (memcmp(t->row[i].mac, mac, 6) == 0) return &t->row[i];
      if (awdl_lru_staler(t->row[i].last_ms, oldest)) {
        oldest = t->row[i].last_ms;
        stalest = i;
      }
    } else if (free_i < 0) {
      free_i = i;
    }
  }
  struct AwdlSrc* s = &t->row[(free_i >= 0) ? free_i : stalest];
  memset(s, 0, sizeof *s);
  s->used = true;
  memcpy(s->mac, mac, 6);
  s->last_ms = now_ms;
  return s;
}

static void awdl_src_add(struct AwdlSrc* s, double innovation_us) {
  s->n++;
  s->s1 += innovation_us;
  s->s2 += innovation_us * innovation_us;
}

/* One phase observation, at one-AW resolution. Saturating, and halved every 512 frames so
   the shape follows the present rather than the whole uptime -- a sender that moves shows
   the move instead of a smear across both positions. */
static void awdl_src_hist(struct AwdlSrc* s, int bin) {
  bin &= (AWDL_SRC_BINS - 1);
  if (s->hist[bin] < AWDL_SRC_CAP) s->hist[bin]++;
  s->hist_n++;
  if ((s->hist_n & AWDL_SRC_DECAY) == 0)
    for (int k = 0; k < AWDL_SRC_BINS; k++) s->hist[k] = (uint8_t)(s->hist[k] >> 1);
}

static double awdl_src_mean(const struct AwdlSrc* s) { return s->n ? s->s1 / s->n : 0.0; }

/* Standard deviation of the innovation.
 *
 * The one-pass form, sqrt(s2/n - mean^2), subtracts two nearly equal large numbers, so it
 * is worth saying what it actually costs here rather than repeating the textbook warning.
 * MEASURED against a two-pass reference over the same samples
 * (tools/test-src.sh):
 *
 *   offset 275,000 us, sd 17 us   -- the worst real sender epoch this project has seen --
 *                                    one-pass 296.832 vs two-pass 296.833. Six digits.
 *   offset 9 us, sd 8.6 us        -- the typical case --  74.6729 vs 74.6729. Exact.
 *   offset 275,000 us, sd 0.006 us -- one-pass goes NEGATIVE (-9.77e-05) and sqrt is NaN.
 *
 * So the one-pass form is fine everywhere this firmware operates: the innovation is
 * wrapped into +/-AWC/2 = +/-524,288 us before it ever gets here, which bounds the term
 * that cancels, and a real sender's spread is tens of microseconds, not hundredths. The
 * clamp below is not fixing an observed failure -- it costs one compare and removes the
 * only way this can return NaN, which matters because the sender it would misprint for is
 * the one furthest out, i.e. the one worth looking at.
 */
static double awdl_src_sd(const struct AwdlSrc* s) {
  if (!s->n) return 0.0;
  double mn = s->s1 / s->n;
  double var = s->s2 / s->n - mn * mn;
  return var > 0.0 ? sqrt(var) : 0.0;
}
