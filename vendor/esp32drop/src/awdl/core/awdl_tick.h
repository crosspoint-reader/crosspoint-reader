/* awdl_tick.h -- when to transmit, decided against the window rather than against a timer.
 *
 * THE THING THIS REPLACES, AND WHY. The first version of this header ran the cadence off
 * relative deadlines -- "15 ms since the last MIF" -- which is what loop() had always done.
 * Simulated against measured blocking and one tick of wake jitter, a 5 ms relative tick
 * delivers 3.43 MIF per window against the 3.58 loop() already manages. It is worse than
 * doing nothing. The reason is that a relative deadline has no idea where the window is:
 * every delay pushes the whole train later, and the frames fall off the end of a window
 * that does not move.
 *
 * So the schedule is ANCHORED to the window. The k-th frame of a run goes out at
 * run_open + k * 15,000 us, an absolute instant. If a pass is late, the missed instants are
 * skipped and COUNTED -- the grid is never re-anchored to "now", because re-anchoring is
 * how a late pass turns into a permanently shifted train.
 *
 * WHAT GATES A MIF: the window is open, and this instant has not been sent. Nothing else.
 * In particular NOT "is there time left for the worst-case injection" -- the 12,000 us
 * guard at the end of every run exists precisely to absorb that injection, whose worst
 * measured value is 7,073 us, a 1.70x margin. Testing for it again on top of the guard
 * double-counts it, and turns the last instant of a width-1 run into a coin flip: it has
 * 463 us of nominal margin, less than the wake jitter.
 *
 * The netif drain is different and DOES get that test, per frame. It has 3.7-15x throughput
 * headroom, so declining a frame costs nothing, and it must also stop before the next MIF
 * instant -- otherwise a mid-window wake front-loads sixteen frames into the shared driver
 * queue directly ahead of the frame that keeps the badge visible.
 *
 * Pure: no clock of its own, no globals, no Serial, no allocation. Everything it needs is
 * an argument, which is what lets tools/test-tick.sh replay a real window schedule against
 * it with injected jitter and injected per-transmit costs.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "awdl_window.h"

struct AwdlTickCfg {
  uint32_t mif_period_us;      /* 15,000 -- see the header comment for how it was chosen */
  uint32_t elect_period_us;    /* 50,000 */
  uint32_t announce_period_us; /* 2,500,000 FLOOR. The real period is ~3.158 s: an
                                  announce may only ride a MIF, MIFs exist only inside the
                                  window, and the window comes round once per 1.048576 s,
                                  so 2,500 ms rounds up to the third cycle. MEASURED. */
  uint32_t tx_worst_us;        /* 7,073 -- the DRAIN admission test, never the MIF's */
  uint8_t drain_budget;        /* frames per pass */
};

/* The shipped cadence. Every number has a measurement behind it. */
#define AWDL_TICK_CFG_DEFAULT {15000, 50000, 2500000, 7073, 16}

struct AwdlTickIn {
  int64_t now_us;   /* monotonic; esp_timer on the target */
  int64_t phase_us; /* position in the AW cycle at now_us */
  bool have_lock;
  bool master_ok; /* an election result exists -- send_mif refuses without one */
  uint16_t ch6_mask;
};

struct AwdlTickOut {
  bool do_elect;
  bool do_mif;
  bool do_announce;       /* implies do_mif: the announce rides a frame that is really sent */
  uint8_t drain_max;      /* how many netif frames this pass may flush */
  int64_t drain_until_us; /* ...and the ABSOLUTE instant it must have stopped by. The count
                             alone is not a limit: the room was tested for ONE frame, and
                             sixteen of them at the measured p90 cost is 53.7 ms against a
                             52,536 us run. The executor must re-test this per frame. */
  int64_t next_wake_us;   /* ABSOLUTE. Never a constant: the sleep is computed from this. */
};

struct AwdlTick {
  struct AwdlTickCfg cfg;
  int64_t next_elect_us;
  int64_t next_announce_us;
  int64_t run_open_us; /* absolute instant this run became usable; the anchor */
  int64_t run_end_us;
  int32_t mif_k;           /* next un-sent instant in this run */
  int64_t last_run_end_us; /* end of the last run we actually visited */
  bool anchored;
  bool have_last_end;
  /* counters -- the acceptance instrument */
  uint32_t n_wake, n_mif, n_announce;
  uint32_t n_mif_late;            /* instants that passed unsent because the pass came too late */
  uint32_t win_total, win_served; /* runs entered; runs with no late instant */
  uint32_t late_wake_us_max;
  /* WHEN something last went wrong, not just how often. n_mif_late and win_total/win_served
     are lifetime totals, so a verdict built on them can be entered but never left: one
     missed window at hour one leaves the device "degraded" for ever, however well it is
     running now. Measured: run=15919/17394 after five hours, reported
     degraded, while the link was busy recovering. A stamp is what lets health say "not any
     more". INT64_MIN means it has never happened. */
  int64_t last_bad_us;
  bool run_had_late;
};

static void awdl_tick_init(struct AwdlTick* t, const struct AwdlTickCfg* cfg) {
  struct AwdlTickCfg d = AWDL_TICK_CFG_DEFAULT;
  t->cfg = cfg ? *cfg : d;
  /* A zero period is not a configuration, it is an uninitialised struct. The late-instant
     loop advances by mif_period_us, so zero makes it spin forever -- which is what an
     un-initialised BSS AwdlTick did. Guarded here AND in the loop itself. */
  if (t->cfg.mif_period_us == 0) t->cfg.mif_period_us = d.mif_period_us;
  if (t->cfg.elect_period_us == 0) t->cfg.elect_period_us = d.elect_period_us;
  t->next_elect_us = t->next_announce_us = 0;
  t->run_open_us = t->run_end_us = 0;
  t->mif_k = 0;
  t->last_run_end_us = 0;
  t->anchored = false;
  t->have_last_end = false;
  t->n_wake = t->n_mif = t->n_announce = t->n_mif_late = 0;
  t->win_total = t->win_served = 0;
  t->late_wake_us_max = 0;
  t->last_bad_us = INT64_MIN;
  t->run_had_late = false;
}

/* Advance an absolute grid past `now` WITHOUT re-anchoring it. A pass that arrives 200 ms
   late must resume on the original phase, not start a new one 200 ms over. */
static int64_t awdl_tick_catchup(int64_t deadline, int64_t now, uint32_t period) {
  if (period == 0) return now;
  if (deadline > now) return deadline;
  int64_t behind = now - deadline;
  return deadline + ((behind / period) + 1) * (int64_t)period;
}

/* `now_us` is not optional. Without it this counted instants that had not happened yet:
   lose the lock 100 us after a run's first frame and it books the instants at +15, +30 and
   +45 ms as LATE, because they are inside the run and unsent. They are neither -- they are
   in the future, and the gate closing is not the cadence being slow. On hardware that makes
   late= grow continuously and win_total inflate many times over the number of runs that
   exist, which destroys the one reading that separates a live cadence from a dead one, and
   makes CADENCE-LATE fire in the first minute forever. An alarm that always fires is an
   alarm nobody reads. The documented case -- a pass landing past run_end -- is unaffected:
   there every remaining instant genuinely is in the past. */
static void awdl_tick_close_run(struct AwdlTick* t, int64_t now_us) {
  if (!t->anchored) return;
  /* Instants this run never got a pass for at all -- the task was asleep, or blocked, right
     past the end of the run. They MUST be counted here and not only on the pass that
     notices them, because there is no such pass: the next wake lands in the next run.
     Without this a run that delivered one frame of four closes as fully served, and
     win_served/win_total reports a perfect 1.0 for a cadence that is barely running --
     which is precisely the reading a DEAD cadence task would produce. */
  while (t->cfg.mif_period_us) {
    int64_t inst = t->run_open_us + (int64_t)t->mif_k * (int64_t)t->cfg.mif_period_us;
    if (inst >= t->run_end_us) break; /* past the end of the run: nothing was owed */
    if (inst >= now_us) break;        /* has not happened yet: future, not late */
    t->n_mif_late++;
    t->run_had_late = true;
    t->last_bad_us = now_us;
    t->mif_k++;
  }
  t->win_total++;
  if (!t->run_had_late)
    t->win_served++;
  else
    t->last_bad_us = now_us;
  t->last_run_end_us = t->run_end_us;
  t->have_last_end = true;
  t->anchored = false;
  t->run_had_late = false;
}

/* Runs that came and went without a single pass.
 *
 * The tick cannot observe these directly -- by definition no pass happened during them --
 * but it can count them, because a gap of more than a whole AW cycle between the end of the
 * last run we visited and the start of the next one means whole cycles went by unvisited.
 *
 * Without this, win_served/win_total reads a PERFECT 1.0 while the cadence sleeps through
 * windows, which is precisely what a dead task looks like and precisely what that ratio
 * exists to detect. Found by a host test, not by reading.
 *
 * It counts one missed run per skipped cycle. For a mask with two ch6 runs per cycle that
 * is an undercount -- but it is never zero when passes are being missed, which is the
 * property the alarm needs. */
static void awdl_tick_count_unvisited(struct AwdlTick* t, int64_t run_open) {
  if (!t->have_last_end || run_open <= t->last_run_end_us) return;
  int64_t missed = (run_open - t->last_run_end_us) / (int64_t)AWC_US;
  if (missed > 0) t->last_bad_us = run_open; /* stamped at the END of the gap: when we noticed */
  while (missed-- > 0) t->win_total++;       /* unserved by construction */
}

/* Is an election due? Asked WITHOUT mutating, so a caller can elect before stepping.
   That ordering matters: the tick decides do_mif from master_ok, and reading master_ok from
   a pre-election snapshot costs the first frame after every master re-acquisition a whole
   election period -- and, in the other direction, lets the tick book an instant as sent
   while the caller then suppresses it because the fresh election came back empty. */
static bool awdl_tick_elect_due(const struct AwdlTick* t, int64_t now_us) {
  return t->next_elect_us == 0 || now_us >= t->next_elect_us;
}

/* One pass. Fires what is due, and says when to come back. */
static void awdl_tick_step(struct AwdlTick* t, const struct AwdlTickIn* in, struct AwdlTickOut* out) {
  const struct AwdlTickCfg* c = &t->cfg;
  out->do_elect = out->do_mif = out->do_announce = false;
  out->drain_max = 0;
  out->drain_until_us = 0;
  t->n_wake++;

  if (t->next_elect_us == 0) t->next_elect_us = in->now_us; /* first pass elects */
  if (in->now_us >= t->next_elect_us) {
    out->do_elect = true;
    t->next_elect_us = awdl_tick_catchup(t->next_elect_us, in->now_us, c->elect_period_us);
  }

  /* INVARIANT (c): no lock, no master, or no mask means no transmit of any kind, and the
     schedule falls back to the election grid. Transmitting on a guess is how 4,125 frames
     once went into a slot nobody was listening on. */
  if (!in->have_lock || !in->master_ok || in->ch6_mask == 0) {
    awdl_tick_close_run(t, in->now_us);
    out->next_wake_us = t->next_elect_us;
    return;
  }

  struct AwdlWindow w;
  awdl_window_init(&w, in->ch6_mask);
  int64_t open_rel = 0, len = 0;
  if (!awdl_window_run_at(&w, in->phase_us, &open_rel, &len)) {
    awdl_tick_close_run(t, in->now_us);
    out->next_wake_us = t->next_elect_us;
    return;
  }
  int64_t run_open = in->now_us + open_rel; /* absolute; <= now while open */
  int64_t run_end = run_open + len;

  /* IS THIS THE RUN WE ARE ALREADY ON?
   *
   * Not `run_open != t->run_open_us`. That was the first version and it is wrong in a way
   * no ideal-clock test can see: run_open is a MEASUREMENT of where the master's window
   * sits on our clock -- it is now_us minus a PLL-derived phase -- so it moves on every
   * single pass. It moves with the PLL's own KP correction, with the master's drift
   * (measured p50 13.2 ppm, p90 19.7, max 80.4 on this hardware), and with g_ts_off32,
   * which the frame path rewrites on every action frame.
   *
   * Under exact equality the tick therefore re-anchored constantly, and the anti-burst path
   * skipped the instant that was due each time. Measured against a live PLL: 87% of ceiling
   * at the p90 drift, with 19,164 late instants and 5,245 "runs" counted in 300 cycles.
   * Against the ideal clock every test in tools/test-tick.sh used -- where now-phase is
   * bit-constant -- it read 100.0%, late 0, 300/300. The whole suite was blind by
   * construction.
   *
   * Identity is the RUN, not the microsecond. Half a MIF period of tolerance: far larger
   * than any drift or jitter can move the anchor between passes, far smaller than the gap
   * to the next run. */
  int64_t anchor_delta = t->anchored ? (run_open - t->run_open_us) : 0;
  if (anchor_delta < 0) anchor_delta = -anchor_delta;
  bool same_run = t->anchored && anchor_delta < (int64_t)c->mif_period_us / 2;

  if (same_run) {
    /* Adopt the fresher measurement rather than keeping the stale one: the window really
       did move, and the instants derived from it should move with it. */
    t->run_open_us = run_open;
    t->run_end_us = run_end;
  } else {
    awdl_tick_close_run(t, in->now_us);
    awdl_tick_count_unvisited(t, run_open);
    t->run_open_us = run_open;
    t->run_end_us = run_end;
    t->anchored = true;
    t->run_had_late = false;
    /* ANTI-BURST. Instants that fell before this pass are not fired retroactively: a badge
       that has just acquired a lock, or has just been given a master, must not empty a
       run's worth of frames into the air at once. Start at the first instant not yet past. */
    t->mif_k = 0;
    if (in->now_us > run_open && c->mif_period_us) {
      int64_t into = in->now_us - run_open;
      t->mif_k = (int32_t)(into / (int64_t)c->mif_period_us);
      if ((int64_t)t->mif_k * c->mif_period_us < into) t->mif_k++;
    }
  }
  /* Refreshed unconditionally, and that is not tidiness. awdl_window_run_at walks BACKWARD
     to a run's first slot, so a mesh that appends a slot to the END of the run we are on
     leaves open_rel bit-identical while len_us grows. Assigning run_end only on re-anchor
     kept the old, shorter end: half the run's instants refused, sixty-odd pointless 1 ms
     wakes in the stranded slot, most of them issuing a full drain -- and late=0 with the
     run closing SERVED, because close_run's own loop is bounded by the same stale value.
     The mesh densifies 1/16 -> 5/16 during a transfer, which is exactly when it bites. */
  t->run_end_us = run_end;

  int64_t inst = t->run_open_us + (int64_t)t->mif_k * c->mif_period_us;

  /* Late instants: passed, inside the run, never sent. Counted, then skipped -- the grid is
     not re-anchored. This counter is the whole reason the task exists; if it is not zero,
     something is holding the pass up. */
  while (c->mif_period_us && inst < in->now_us && inst < t->run_end_us) {
    int64_t next = inst + (int64_t)c->mif_period_us;
    if (next > in->now_us) break; /* `inst` is the one due NOW */
    t->n_mif_late++;
    t->run_had_late = true;
    t->last_bad_us = in->now_us;
    uint32_t lateness = (uint32_t)(in->now_us - inst);
    if (lateness > t->late_wake_us_max) t->late_wake_us_max = lateness;
    t->mif_k++;
    inst = next;
  }

  bool open_now = awdl_window_open_at(&w, in->phase_us);
  if (open_now && inst <= in->now_us && inst < t->run_end_us) {
    out->do_mif = true;
    t->n_mif++;
    uint32_t lateness = (uint32_t)(in->now_us - inst);
    if (lateness > t->late_wake_us_max) t->late_wake_us_max = lateness;
    t->mif_k++;
    if (t->next_announce_us == 0 || in->now_us >= t->next_announce_us) {
      out->do_announce = true;
      t->n_announce++;
      t->next_announce_us = in->now_us + (int64_t)c->announce_period_us;
    }
    inst = t->run_open_us + (int64_t)t->mif_k * c->mif_period_us;
  }

  /* The drain. Admitted only with room for the worst measured injection, and only while
     that room ends before the next MIF instant -- otherwise a mid-run wake front-loads the
     budget into the shared driver queue directly ahead of the frame that keeps us visible. */
  if (open_now) {
    int64_t remaining = awdl_window_remaining_at(&w, in->phase_us);
    int64_t until_next_mif = (inst < t->run_end_us) ? (inst - in->now_us) : remaining;
    int64_t room = (remaining < until_next_mif) ? remaining : until_next_mif;
    if (room > (int64_t)c->tx_worst_us) {
      out->drain_max = c->drain_budget;
      out->drain_until_us = in->now_us + room - (int64_t)c->tx_worst_us;
    }
  }

  /* Next wake: the earlier of the next transmit instant and the next election. If the run
     has no instants left, the next thing that matters in it is its end. */
  int64_t wake = (inst < t->run_end_us) ? inst : t->run_end_us;
  if (!open_now && open_rel > 0) wake = run_open; /* not open yet: wake at the edge */
  if (t->next_elect_us < wake) wake = t->next_elect_us;
  if (wake <= in->now_us) wake = in->now_us + 1;
  out->next_wake_us = wake;
}
