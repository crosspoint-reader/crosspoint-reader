/* awdl_status.h -- what a library user is allowed to ask, and how to read the answer.
 *
 * This is a PULL API on purpose. There is no log sink and no callback here: library tasks
 * run at priorities where a user's code must never execute, so nothing this header exposes
 * can put the caller on the cadence task, the frame task or the TLS task. You ask; you are
 * answered from a snapshot.
 *
 * The hard part of a status API is not the fields, it is refusing to make the caller an
 * expert. "Is my badge discoverable right now" has one answer, not eleven, and a user who
 * has to compare mif_sent against a ceiling they compute themselves from a channel mask
 * will get it wrong. So the derived questions are answered here, tested, once -- and the
 * raw fields are there underneath for anyone who wants them.
 *
 * Dependency-free integer C plus the window arithmetic, so the same code compiles into the
 * firmware and into tools/test-status.sh.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "awdl_window.h"

/* One answer to "how is it going", ordered so that > is worse. */
enum {
  AWDL_HEALTH_OK = 0,   /* an elected master, a locked estimator, frames going out */
  AWDL_HEALTH_DEGRADED, /* transmitting, but missing instants or losing lock quality */
  AWDL_HEALTH_SILENT,   /* everything looks right and nothing has gone out lately */
  AWDL_HEALTH_NO_LOCK,  /* a master, but the phase estimator is not tracking it */
  AWDL_HEALTH_NO_MASTER /* nobody to sync to: nothing is transmitted, by design */
};

/* How long the transmit path may be quiet before the device is treated as invisible.
 *
 * Both bounds are measured, and the value sits between them:
 *   - A width-1 channel mask opens a 52,536 us window once per 1,048,576 us cycle, so a
 *     PERFECTLY healthy device is silent for about 996 ms between windows. Anything at or
 *     below one second would alarm on normal operation.
 *   - macOS ages a silent peer out of its AWDL table in 6-8 s. Anything above that reports
 *     health after the peer has already dropped us.
 * 3 s is 3x the natural gap and half the aging floor. */
#define AWDL_SILENCE_MAX_MS 3000u
/* Ten AW cycles (AWC is 1.048576 s). Long enough that a single missed window still shows
   up for a human watching the STAT line, short enough that a device which has recovered
   says so within a few seconds rather than at the next reboot. */
#define AWDL_DEGRADE_MAX_MS 10000u

struct AwdlStatus {
  /* --- the link ------------------------------------------------------------------- */
  bool have_master;
  bool locked;
  uint8_t master[6];
  uint32_t master_metric;
  int32_t peers;         /* masters heard recently */
  uint32_t no_master_ms; /* how long we have had nobody to sync to, RIGHT NOW.
                            0 whenever a master is elected. Not a lifetime total:
                            it used to be assigned the accumulated dwell of
                            election-FALLBACK episodes, which are measured while a
                            master IS present and which never return to zero. Name,
                            comment and implementation disagreed three ways. The
                            fallback figure is real and still published, as
                            AwdlDiag::elect_fb / elect_fb_ms. */

  /* --- the transmit window -------------------------------------------------------- */
  uint16_t ch6_mask; /* bit k = AWC slot k is ch6. 0 means no authority */
  int8_t ch6_slot;   /* first ch6 slot, or AWDL_SLOT_NONE */
  uint8_t win_state; /* AWDL_WS_* */

  /* --- what the cadence actually did ---------------------------------------------- */
  uint32_t mif_sent;         /* cumulative since begin(). Says a frame HAS gone out,
                                never that one is going out NOW -- see ms_since_mif */
  uint32_t ms_since_mif;     /* since the last frame the radio ACCEPTED.
                                UINT32_MAX = not one yet */
  uint32_t mif_late;         /* instants that passed unsent. Should be 0 */
  uint32_t ms_since_degrade; /* since the last late instant or unserved window.
                                UINT32_MAX when it has never happened. Lifetime counts
                                say a device HAS been unwell; only this says it still is */
  uint32_t win_served, win_total;
  uint32_t tx_errors;
  uint32_t late_wake_us_max;

  /* --- estimator quality, for deciding whether to trust the link ------------------- */
  float wander_us;    /* smoothed-track jitter; "locked" means this is small */
  float noise_rms_us; /* raw measurement noise */
  float drift_ppm;    /* our clock against the master's */
  uint32_t samples;
};

/* Frames per second the current window PERMITS. Not a target and not a promise: it is what
   the mask allows, so mif_sent can be judged without the caller re-deriving the AWDL
   timing. 0 when there is no window.
 *
 * Counted as instants per cycle rather than assumed, because a mask is not one block: 0x3b00
 * is two ch6 runs, slots 8-9 and 11-13, each with its own lead and guard, and treating it as
 * one gives 8 where the answer is 21. */
static float awdl_status_mif_ceiling(uint16_t mask, uint32_t mif_period_us) {
  if (!mask || !mif_period_us) return 0.0f;
  struct AwdlWindow w;
  awdl_window_init(&w, mask);
  int total = 0;
  int64_t p = 0;
  for (int guard = 0; guard < AWDL_SLOTS && p < AWC_US; guard++) {
    int64_t open_rel, len;
    if (!awdl_window_run_at(&w, p, &open_rel, &len)) break;
    if (p + open_rel >= AWC_US) break; /* that run belongs to the next cycle */
    int n = 0;
    while ((int64_t)n * mif_period_us < len) n++;
    total += n;
    p = p + open_rel + len + 1;
  }
  return (float)total * 1000000.0f / (float)AWC_US;
}

/* How long the CURRENT episode has lasted, or 0 when there is no episode.
 *
 * Three lines, and they are here rather than in the port for two reasons. It is the shape
 * both no_master_ms and ms_since_mif need, and getting it wrong is how no_master_ms came
 * to report a lifetime total in the first place -- "how long has this been true" and "how
 * much of this has there ever been" are different questions with the same units.
 *
 * WRAP-EXACT, and that is the testable part. The timestamps are uint32 milliseconds
 * because they cross a task-priority boundary and a 64-bit read on a 32-bit core can tear;
 * that choice buys atomicity and costs a rollover every 49.7 days. Unsigned subtraction
 * gets the right answer across it, and the host suite pins that rather than trusting it. */
static uint32_t awdl_episode_ms(bool active, uint32_t since_ms, uint32_t now_ms) {
  return active ? (uint32_t)(now_ms - since_ms) : 0u;
}

/* THE question. Everything else on this struct is detail.
 *
 * Discoverable means: somebody to sync to, a phase we are tracking, a window we have
 * authority for, and frames LEAVING RIGHT NOW. Not "the radio is on" -- this project has
 * twice shipped firmware that reported perfect health while transmitting nothing.
 *
 * ⚠️ FIXED, and the bug is worth keeping in view because the comment above it
 * was already correct. This used to test `mif_sent > 0`. mif_sent is CUMULATIVE, so one
 * frame at boot made it true for ever -- and the failure it was written to catch is
 * exactly the one it could not see: a dead cadence task freezes mif_late and
 * win_served/win_total at whatever ratio they held, so those look perfect too, while
 * gauge and masters stay healthy because the receive path is untouched. Every indicator
 * green, nothing on the air, macOS dropping us after 6-8 s. The intent was right and the
 * predicate could not implement it; recency is what makes it implementable. */
static bool awdl_status_discoverable(const struct AwdlStatus* s) {
  return s->have_master && s->locked && s->ch6_mask != 0 && s->mif_sent > 0 && s->ms_since_mif <= AWDL_SILENCE_MAX_MS;
}

/* How is it going, in one value.
 *
 *   ms_since_degrade   a late instant, or a whole availability window that went unserved,
 *                      within the last AWDL_DEGRADE_MAX_MS
 *
 * Exact rather than proportional: these are things that should not happen at all, and a
 * percentage of them would only hide the first one.
 *
 * RECENT rather than cumulative, and that is the second time this file has had to learn
 * it -- see the note on awdl_status_discoverable above, which had the identical bug. This
 * used to test `mif_late > 0 || win_served < win_total`, all three of them lifetime
 * totals, so the verdict could be entered and never left: one missed window in the first
 * minute left the device "degraded" for the rest of its uptime no matter how well it ran
 * afterwards. Measured: run=15919/17394 after five hours, reported degraded.
 * A health reading that cannot say "not any more" is not a health reading; it is a
 * memory. */
static int awdl_status_health(const struct AwdlStatus* s) {
  if (!s->have_master || s->ch6_mask == 0) return AWDL_HEALTH_NO_MASTER;
  if (!s->locked) return AWDL_HEALTH_NO_LOCK;
  /* Checked BEFORE the counters, because the counters are what freezes. A stalled or dead
     transmit path leaves mif_late and win_served/win_total exactly as they were, so they
     report OK for ever; only the clock keeps moving. */
  if (s->ms_since_mif > AWDL_SILENCE_MAX_MS) return AWDL_HEALTH_SILENT;
  if (s->ms_since_degrade <= AWDL_DEGRADE_MAX_MS) return AWDL_HEALTH_DEGRADED;
  return AWDL_HEALTH_OK;
}

static const char* awdl_status_health_name(int h) {
  switch (h) {
    case AWDL_HEALTH_OK:
      return "ok";
    case AWDL_HEALTH_DEGRADED:
      return "degraded";
    case AWDL_HEALTH_SILENT:
      return "silent";
    case AWDL_HEALTH_NO_LOCK:
      return "no-lock";
    default:
      return "no-master";
  }
}
