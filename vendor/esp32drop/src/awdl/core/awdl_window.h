/* awdl_window.h -- the AWDL wire and timing constants.
 *
 * Split out of awdl_sync.h because awdl_sync.h is C++ only (its structs use default
 * member initialisers) while everything that needs these numbers -- the frame builder,
 * the window arithmetic, their host tests -- is dependency-free C in the house style.
 * Duplicating the constants into each of those would be the obvious mistake: AW_US and
 * AWC_US are load-bearing for the gauge, the TX gate and the MIF's live timing fields
 * at once, and two copies is one copy that can drift.
 *
 * The window ARITHMETIC (slot/mask/phase, and the next-window-opens-in function the
 * cadence needs) lives here as well, below the constants.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* ---- timing -----------------------------------------------------------------
 * One AW is 16 TU; one availability-window cycle (AWC) is 64 AW. The chanseq
 * divides the AWC into 16 slots of 4 AW each, so one slot is 65,536 us and the mesh
 * typically grants exactly one of them on ch6 -- a 6.25% raw duty cycle, 5.01% after
 * the lead-in and end guard. Every latency budget in this project derives from that.
 *
 * AWC_US == 1048576 == 2^20 exactly, and 2^32 is a multiple of 2^20, so modular
 * arithmetic on 32-bit rx timestamps is exact across their wrap. handle_sync relies
 * on this; do not "tidy" these into non-power-of-two values.
 */
static const int64_t TU_US = 1024; /* one time unit          */
static const int64_t TU_PER_AW = 16;
static const int64_t AW_PER_AWC = 64;
static const int64_t AW_US = TU_US * TU_PER_AW;   /* 16384 us  (one AW)     */
static const int64_t AWC_US = AW_US * AW_PER_AWC; /* 1048576 us (one cycle) */

/* ---- 802.11 / AWDL wire constants ------------------------------------------- */
static const uint8_t AWDL_BSSID[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
static const uint8_t APPLE_VENDOR[4] = {0x7f, 0x00, 0x17, 0xf2};
static const uint8_t PROBE_CHANNEL = 6;

/* ---- action-frame TLV tags --------------------------------------------------- */
enum {
  TLV_SERVICE_RESPONSE = 0x02,
  TLV_SYNC_PARAMS = 0x04,
  TLV_ELECTION_PARAMS = 0x05,
  TLV_SERVICE_PARAMS = 0x06,
  TLV_HT_CAPABILITIES = 0x07,
  TLV_DATA_PATH_STATE = 0x0c,
  TLV_ARPA = 0x10,
  TLV_CHAN_SEQ = 0x12,
  TLV_VERSION = 0x15,
  TLV_ELECTION_V2 = 0x18, /* 24: modern election params, carries master_counter */
};

/* ---- window arithmetic ------------------------------------------------------
 *
 * The chanseq divides the AWC into 16 slots of 4 AW. Slots reading channel 6 are the
 * only time a peer can hear us, so this arithmetic is the transmit gate, and it is the
 * only thing standing between the badge and being invisible: gate too tightly and the
 * MIF never leaves, gate on the wrong slot and it leaves into an empty window. Both have
 * happened. So it lives here, as pure functions of (phase, mask), exercised on a host
 * against a scriptable clock rather than only on the device.
 *
 * `phase` is the position within the AWC, in microseconds, that the caller derives from
 * the PLL. Keeping it an argument is what makes this testable at all -- the clock and the
 * lock live in the port.
 *
 * LEAD and GUARD apply only at the edges of a RUN of ch6 slots, never between adjacent
 * ones: the mesh densifies ch6 under traffic (1/16 to 5/16 observed during a transfer)
 * and treating each slot in a run as separately guarded would throw that airtime away.
 */
#define AWDL_SLOTS 16
#define AWDL_SLOT_US (4 * AW_US) /* 65,536 us */
#define AWDL_WIN_LEAD_US 1000    /* settle after a run opens */
#define AWDL_WIN_GUARD_US                        \
  12000 /* stop before a run closes: frame build \
           plus esp_wifi_80211_tx latency, so an \
           initiated TX cannot land in the next  \
           (5GHz) slot */

struct AwdlWindow {
  uint16_t mask; /* bit k set = AWC slot k is ch6 */
  int64_t lead_us;
  int64_t guard_us;
};

static void awdl_window_init(struct AwdlWindow* w, uint16_t mask) {
  w->mask = mask;
  w->lead_us = AWDL_WIN_LEAD_US;
  w->guard_us = AWDL_WIN_GUARD_US;
}

/* Is the window open at this phase? False whenever the mask is empty -- no elected
   master means no authority for where the window is, and transmitting on a guess is how
   4,125 frames once went into an empty slot. */
static bool awdl_window_open_at(const struct AwdlWindow* w, int64_t phase) {
  if (!w->mask) return false;
  if (phase < 0 || phase >= AWC_US) return false;
  int s = (int)(phase / AWDL_SLOT_US);
  if (s < 0 || s >= AWDL_SLOTS) return false;
  if (!(w->mask & (uint16_t)(1u << s))) return false;
  int64_t into = phase - (int64_t)s * AWDL_SLOT_US;
  bool prev6 = (w->mask & (uint16_t)(1u << ((s + AWDL_SLOTS - 1) & (AWDL_SLOTS - 1)))) != 0;
  bool next6 = (w->mask & (uint16_t)(1u << ((s + 1) & (AWDL_SLOTS - 1)))) != 0;
  if (!prev6 && into < w->lead_us) return false;
  if (!next6 && into >= AWDL_SLOT_US - w->guard_us) return false;
  return true;
}

/* Microseconds until the window next opens: 0 if it is open now, AWC_US if there is no
   mask at all. This is what a caller about to block needs -- "is it open" is not enough
   to decide whether a 250 ms write is safe to start. */
static int64_t awdl_window_next_at(const struct AwdlWindow* w, int64_t phase) {
  if (!w->mask) return AWC_US;
  if (phase < 0 || phase >= AWC_US) return AWC_US;
  if (awdl_window_open_at(w, phase)) return 0;
  /* The window can only OPEN at the lead point of a slot that STARTS a run. A ch6 slot
     whose predecessor is also ch6 opens nothing: the window was already open coming into
     it, or it was shut and stays shut until the run's own start comes round again.
     Taking the nearest ch6 slot instead of the nearest run START was wrong, and wrong in
     the dangerous direction -- it reported more slack than there was, to a caller whose
     entire reason for asking is to decide whether it has room to block. */
  int64_t best = AWC_US;
  for (int k = 0; k < AWDL_SLOTS; k++) {
    if (!(w->mask & (uint16_t)(1u << k))) continue;
    if (w->mask & (uint16_t)(1u << ((k + AWDL_SLOTS - 1) & (AWDL_SLOTS - 1)))) continue; /* mid-run: not an opening */
    int64_t d = ((int64_t)k * AWDL_SLOT_US + w->lead_us) - phase;
    if (d <= 0) d += AWC_US;
    if (d < best) best = d;
  }
  return best;
}

/* Every slot in a channel sequence that carries `channel`, as a bitmask. */
static uint16_t awdl_chanseq_mask_of(const uint8_t slot[AWDL_SLOTS], uint8_t channel) {
  uint16_t m = 0;
  for (int i = 0; i < AWDL_SLOTS; i++)
    if (slot[i] == channel) m |= (uint16_t)(1u << i);
  return m;
}
/* The first such slot, or -1. Diagnostic anchor only: the mask is what gates TX. */
static int awdl_chanseq_first_of(const uint8_t slot[AWDL_SLOTS], uint8_t channel) {
  for (int i = 0; i < AWDL_SLOTS; i++)
    if (slot[i] == channel) return i;
  return -1;
}

/* ---- the published window state ---------------------------------------------
 *
 * Everything the transmit gate needs, in ONE object that is copied under a lock.
 *
 * Why an object rather than the six globals it replaces. The publisher writes the PLL
 * fields and then, about twenty lines later, the channel mask -- and a reader that lands
 * between the two pairs a fresh phase with the PREVIOUS election's mask, which aims the
 * transmit window at a slot the new master does not listen on. Today both writer and
 * readers are on core 1 at priority 1, so the gap is only reachable via preemption by
 * awdl_proc; once the cadence task exists the publisher is priority 2 and the loop()-side
 * readers are priority 1, and a reader preempted mid-copy becomes routine.
 *
 * The doubles are the sharp edge. A double torn WITHIN one exponent is off by at most half
 * a microsecond and nobody would ever notice. Torn ACROSS an exponent change -- new
 * exponent, stale mantissa -- it is unbounded, up to a whole 1,048,576 us cycle, and the
 * PLL's fmod wrap makes exponent crossings routine near zero. That is not a tear you
 * recover from: it is a transmit into an empty slot, which is the failure this project has
 * already spent 4,125 frames on.
 *
 * INVARIANT W1: ch6_mask == 0 if and only if state == AWDL_WS_VOID. No mask means no
 * authority for where the window is, and transmitting on a guess is exactly how those
 * 4,125 frames happened.
 */
enum { AWDL_WS_VOID = 0, AWDL_WS_LOCKED = 1, AWDL_WS_COAST = 2 };

/* The slot value that means "we do not have one". Explicit, because the published object
   is zero-initialised and slot 0 is a perfectly valid slot -- so a zeroed field reads as a
   real answer rather than as an absent one. */
#define AWDL_SLOT_NONE (-1)

struct AwdlWinState {
  double phase, freq;    /* the PLL, in the rx_ts domain */
  uint32_t tlast;        /* rx_ts of the last PLL update */
  uint32_t lock_rx;      /* rx_ts when a candidate was last actually present */
  uint16_t ch6_mask;     /* bit k set = AWC slot k is ch6 -- THIS is what the gate uses */
  uint16_t meas_mask;    /* the histogram's opinion, and the chanseq's: the cross-check */
  uint16_t chanseq_mask; /* pair. They are published WITH the mask they were compared
                            against, so the MASK diagnostic cannot report three values
                            sampled at three different times and call it a disagreement. */
  int8_t ch6_slot;       /* the FIRST ch6 slot; a diagnostic anchor, not the gate */
  bool have_lock;        /* what the gate consults */
  uint8_t state;         /* published and counted; see awdl_winstate_step */
};

/* What the state should become. Pure, so the policy can be argued about against fixtures.
 *
 * COAST is DELIBERATELY not wired to the gate yet. Coasting the window without also
 * coasting the frame delivers nothing -- send_mif refuses an election-less Selection by
 * design, and this struct carries none of the fields the MIF builder needs -- so a coast
 * that "works" would report success while transmitting zero frames, which is the
 * quiet-invisibility failure this firmware is most prone to. Publish it, count it, measure
 * how far the coasted prediction drifts from the first real measurement after it, and
 * decide with that in hand.
 */
/* The state for a freshly-decided window, with invariant W1 enforced.
 *
 * Separate from awdl_winstate_step because W1 -- no mask, no state -- is a property of the
 * PUBLISHED object rather than of the transition, and because enforcing it in the caller
 * meant it was not covered by any test. It is now. A COAST published with an empty mask
 * would claim the badge is coasting a window whose position it does not know. */
static uint8_t awdl_winstate_next(uint8_t prev, bool have_candidate, uint16_t mask, uint32_t now_rx, uint32_t lock_rx,
                                  uint32_t coast_max_us);

/* Zeroed is not the same as empty: ch6_slot 0 is a real slot. */
static void awdl_winstate_init(struct AwdlWinState* w) {
  w->phase = w->freq = 0;
  w->tlast = w->lock_rx = 0;
  w->ch6_mask = w->meas_mask = w->chanseq_mask = 0;
  w->ch6_slot = AWDL_SLOT_NONE;
  w->have_lock = false;
  w->state = AWDL_WS_VOID;
}

static uint8_t awdl_winstate_step(uint8_t prev, bool have_candidate, uint32_t now_rx, uint32_t lock_rx,
                                  uint32_t coast_max_us) {
  if (have_candidate) return AWDL_WS_LOCKED;
  if (prev == AWDL_WS_VOID) return AWDL_WS_VOID; /* never coast from nothing */
  return ((uint32_t)(now_rx - lock_rx) <= coast_max_us) ? AWDL_WS_COAST : AWDL_WS_VOID;
}

/* Microseconds remaining in the window at this phase, 0 if it is not open.
 *
 * The complement of awdl_window_next_at, and the one a caller about to spend time needs:
 * "is it open" cannot answer "will it still be open when I finish". Property, exhaustively
 * checked in the host suite: remaining > 0 if and only if the window is open. */
static int64_t awdl_window_remaining_at(const struct AwdlWindow* w, int64_t phase) {
  if (!awdl_window_open_at(w, phase)) return 0;
  int s = (int)(phase / AWDL_SLOT_US);
  /* Walk forward while the run continues. Counting FORWARD from the current slot and
     letting the end run past AWC_US is what makes a run that wraps 15 -> 0 work; the first
     version of this stopped dead at the wrap and returned 0 while the window was wide
     open, for 64,536 us of every cycle at mask 0x8001. A property test over every
     microsecond of eight mask shapes is what caught it. */
  int extra = 0;
  while (extra < AWDL_SLOTS - 1) {
    int nxt = (s + extra + 1) & (AWDL_SLOTS - 1);
    if (!(w->mask & (uint16_t)(1u << nxt))) break;
    extra++;
  }
  int64_t run_end = (int64_t)(s + extra + 1) * AWDL_SLOT_US; /* may exceed AWC_US */
  /* The guard is subtracted only where the run really ends. A mask whose every slot is ch6
     never closes, so it never gets a guard -- and its "remaining" is a floor, at least one
     slot and up to a full cycle ahead, which is the honest answer to a caller asking how
     long it may spend. */
  if (extra < AWDL_SLOTS - 1) run_end -= w->guard_us;
  int64_t r = run_end - phase;
  return r > 0 ? r : 0;
}

/* The usable ch6 run containing, or next following, `phase`.
 *
 * This is what a window-ANCHORED transmit schedule needs and what neither of the other two
 * accessors can give it. "How long until it opens" and "how long is left" both answer
 * questions about NOW; a schedule that puts its k-th frame at run_open + k*period needs the
 * run's origin, which may already be in the past.
 *
 * So `*open_rel` is a SIGNED offset from `phase` to the first usable microsecond of the
 * run: zero or negative when the window is already open, positive when it is not. Signed
 * and relative on purpose -- an absolute phase would have to wrap, and the caller would
 * then have to un-wrap it to add k*period, which is where an off-by-one-cycle bug lives.
 *
 * `*len_us` is the run's usable length: lead removed at the start, guard removed at the
 * end, so `open_rel + len_us` is exactly where transmitting must stop.
 *
 * Returns false when the mask is empty -- no mask, no authority, no schedule.
 */
static bool awdl_window_run_at(const struct AwdlWindow* w, int64_t phase, int64_t* open_rel, int64_t* len_us) {
  if (!w->mask) return false;

  if (!awdl_window_open_at(w, phase)) {
    int64_t d = awdl_window_next_at(w, phase);
    int64_t p2 = (phase + d) % AWC_US;
    *open_rel = d;
    *len_us = awdl_window_remaining_at(w, p2);
    return *len_us > 0;
  }

  /* Open: walk back to the first slot of this run. The whole-cycle mask never has a first
     slot, and correspondingly never has a lead to skip. */
  int s = (int)(phase / AWDL_SLOT_US);
  int back = 0;
  while (back < AWDL_SLOTS - 1) {
    int prv = (s - back - 1) & (AWDL_SLOTS - 1);
    if (!(w->mask & (uint16_t)(1u << prv))) break;
    back++;
  }
  int64_t run_start = (int64_t)(s - back) * AWDL_SLOT_US; /* may be negative */
  int64_t usable_start = run_start + ((back < AWDL_SLOTS - 1) ? w->lead_us : 0);
  int64_t rem = awdl_window_remaining_at(w, phase);
  *open_rel = usable_start - phase; /* <= 0 */
  *len_us = (phase + rem) - usable_start;
  return *len_us > 0;
}

static uint8_t awdl_winstate_next(uint8_t prev, bool have_candidate, uint16_t mask, uint32_t now_rx, uint32_t lock_rx,
                                  uint32_t coast_max_us) {
  uint8_t st = awdl_winstate_step(prev, have_candidate, now_rx, lock_rx, coast_max_us);
  return (mask == 0) ? (uint8_t)AWDL_WS_VOID : st; /* invariant W1 */
}
