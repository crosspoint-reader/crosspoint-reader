#if defined(ESP32DROP_SENDER)
/* ad_cycle.cpp -- see ad_cycle.h. */
#include "ad_cycle.h"

#include <Arduino.h>

#include "../ESP32AWDL.h" /* re-exports awdl/port; the layer rule wants the umbrella */
#include "../ble/BLEAdvMin.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

/* The wake advertisement. Every byte of it is measured off two real Apple devices, except
 * the eight identifier bytes, which are synthetic on purpose.
 *
 * 0x17 = 23 = the type byte plus the 22 that follow. Omitting it cost four cycles of
 * silent failure: the controller accepted the data, bleadv_begin() returned success, and
 * macOS never woke, because a scanner reads a leading 0xFF as an AD element length of 255
 * and throws the advertisement away. */
const uint8_t AD_WAKE_ADV[24] = {0x17, 0xFF, 0x4C, 0x00, /* manufacturer data, Apple                            */
                                 0x05, 0x12,             /* Continuity type 0x05 = AirDrop, 18 bytes follow      */
                                 0x40,                   /* flags, as both captured devices carry                */
                                 0xE5, 0x32, 0x0D, 0x00, 0x00,
                                 0x00, 0x00, 0x03,       /* version, as both captured devices carry              */
                                 0xA1, 0x7C, 0x3B, 0x90, /* SYNTHETIC -- identifies nobody                       */
                                 0x5E, 0x04, 0xD2, 0x68, 0x00};

static int g_last_err;
int ad_cycle_last_error(void) { return g_last_err; }

static struct AdCycleCfg g_cfg;
static int g_phase = AD_CYCLE_OFF;
static uint32_t g_t0; /* millis() when the current phase began */
static bool g_hold;
static bool g_ble_up, g_awdl_up;
/* WHEN THE ADVERTISEMENT STOPPED, which is the instant every deadline that matters is
   measured from. The peer's listener closes a fixed time after this, not after AWDL comes
   up or after WATCHING begins, and the old code bounded the watch from the wrong instant.
   See the note on AD_WAKE_SAFE_MS in ad_cycle.h. */
static uint32_t g_advert_stop_ms;
static uint32_t g_lock_started_ms; /* for the one number that says whether (a) works */
/* WATCHING pushes g_t0 forward on every poll while a transfer holds the cycle open, so
   now - g_t0 is "time since the hold ended", not "time spent watching". The instrumented
   line said watched_ms=2993 for a window that had been open 16.8 s. Keep the real start. */
static uint32_t g_watch_started_ms;

/* ONE LINE PER PHASE CHANGE, staged like every other diagnostic in this library -- it never
 * writes to Serial, the sketch decides where the lines go.
 *
 * It exists because this module's two deadlines were changed on the strength of an
 * argument, and an argument is not a measurement. lock_ms was made a cap rather than a
 * wait, and the watch was re-based onto the instant the advertisement stops; neither shows
 * up in the sketch's own output, so a cycle that still attempted a connect past the iOS
 * cliff would look exactly like one that did not. since_advert is the number the peer's
 * listener is keeping time against, so it is the number that has to be on the line. */
static void cycle_log(const char* what, uint32_t a, const char* aname, uint32_t b, const char* bname) {
  char line[96];
  snprintf(line, sizeof line, "CYCLE %s %s=%lu %s=%lu", what, aname, (unsigned long)a, bname, (unsigned long)b);
  awdl_diag_stage(line, nullptr, 0);
}

static uint32_t phase_len(void) {
  switch (g_phase) {
    case AD_CYCLE_WAKING:
      return g_cfg.advertise_ms;
    case AD_CYCLE_LOCKING:
      return g_cfg.lock_ms;
    case AD_CYCLE_WATCHING:
      return g_cfg.watch_ms;
    default:
      return 0;
  }
}

static void enter(int phase) {
  g_phase = phase;
  g_t0 = millis();
}

int ad_cycle_begin(const struct AdCycleCfg* cfg) {
  if (cfg)
    g_cfg = *cfg;
  else
    memset(&g_cfg, 0, sizeof g_cfg);
  if (!g_cfg.advertise_ms) g_cfg.advertise_ms = AD_WAKE_ADVERTISE_MS;
  if (!g_cfg.lock_ms) g_cfg.lock_ms = AD_CYCLE_LOCK_MS;
  if (!g_cfg.watch_ms) g_cfg.watch_ms = AD_CYCLE_WATCH_MS;
  g_hold = false;
  g_ble_up = false;
  g_awdl_up = false;
  enter(AD_CYCLE_WAKING);
  g_t0 = millis() - g_cfg.advertise_ms; /* fall straight into the first BLE start */
  return 0;
}

void ad_cycle_poll(void) {
  if (g_phase == AD_CYCLE_OFF) return;
  const uint32_t now = millis();
  const uint32_t len = phase_len();
  const bool due = (uint32_t)(now - g_t0) >= len;

  switch (g_phase) {
    case AD_CYCLE_WAKING:
      if (!g_ble_up) {
        /* A controller that will not start now is not a permanent failure. Skip the round
           and try the next one rather than wedging the sketch. */
        bleadv_config_t bc = bleadv_config_default();
        bc.interval_ms = 30; /* the rate both captured Apple devices advertise at */
        /* THE CONTROLLER'S OWN MEMORY IS THE EVIDENCE, and until now this only SAID so.
           begin() and enable() both returning OK is not the same thing as a radio that is
           advertising: if the controller never came up, the peer never wakes, its listener
           never binds :8770, and every connect this cycle makes times out -- while every
           return value still reads success and ad_cycle_last_error() stays 0. That failure
           is indistinguishable from "nobody is there", which is exactly the wrong thing for
           it to look like. The controller costs about 26 KB of internal DRAM
           (26,452 B measured standalone, 26,620 B in this tree), so
           the heap it takes is the one witness that cannot be faked by a return code. */
        const uint32_t heap_before = esp_get_free_internal_heap_size();
        const int rc = bleadv_begin(AD_WAKE_ADV, sizeof AD_WAKE_ADV, &bc);
        if (rc != BLEADV_OK) {
          g_last_err = rc;
          enter(AD_CYCLE_WAKING);
          return;
        }
        bleadv_enable(true);
        const uint32_t heap_after = esp_get_free_internal_heap_size();
        const uint32_t took = heap_before > heap_after ? heap_before - heap_after : 0;
        cycle_log("ble", took, "controller_bytes", heap_after, "heap_left");
        if (took < AD_BLE_MIN_BYTES) {
          /* Report it as a failure of the wake rather than letting the whole cycle run
             blind. AD_BLE_MIN_BYTES is half the measured cost: comfortably below anything
             a real controller takes, comfortably above measurement noise. */
          g_last_err = AD_CYCLE_E_BLE_SILENT;
          bleadv_enable(false);
          bleadv_end();
          enter(AD_CYCLE_WAKING);
          return;
        }
        g_last_err = 0;
        g_ble_up = true;
        enter(AD_CYCLE_WAKING);
        return;
      }
      if (!due) return;
      /* BLE OUT BEFORE AWDL IN, with no overlap: the two cannot both be resident, and the
         order is not a preference. Releasing after WiFi has started leaves the controller's
         memory unavailable for the rest of the run. */
      bleadv_enable(false);
      bleadv_end();
      g_ble_up = false;
      g_advert_stop_ms = millis(); /* every deadline below is measured from here */
      {
        const int arc = (int)awdl_begin();
        if (arc != 0) {
          g_last_err = arc;
          enter(AD_CYCLE_WAKING);
          return;
        }
        g_last_err = 0;
      }
      g_awdl_up = true;
      g_lock_started_ms = millis();
      cycle_log("locking", (uint32_t)(millis() - g_advert_stop_ms), "since_advert",
                (uint32_t)(millis() - g_lock_started_ms), "awdl_up_ms");
      enter(AD_CYCLE_LOCKING);
      return;

    case AD_CYCLE_LOCKING:
      /* Lock is worth waiting for but not worth requiring. A send has succeeded on a cycle
         whose status read said locked=0 immediately beforehand, so this is a settling
         delay, not a gate.

         SO STOP PAYING FOR IT ONCE IT ARRIVES. lock_ms used to be spent in full, every
         cycle, and against an iPhone that is most of the budget: the peer's listener closes
         about 25 s after the advertisement stops, so twelve fixed seconds of settling left
         thirteen to find somebody in -- and the header's own arithmetic, which assumed the
         watch began when the advertisement stopped, was describing a window that did not
         exist. Now lock_ms is a CAP: when the estimator says locked, the settling is over
         and there is nothing left to wait for. */
      {
        struct AwdlStatus st;
        awdl_status_read(&st);
        if (!st.locked && !due) return;
        /* locked=1 means (a) did its job: the cap was not spent. locked=0 with the cap
           reached is the old behaviour, and seeing which one happens is the point. */
        cycle_log(st.locked ? "watching/locked" : "watching/capped", (uint32_t)(millis() - g_lock_started_ms),
                  "lock_took_ms", (uint32_t)(millis() - g_advert_stop_ms), "since_advert");
      }
      g_watch_started_ms = millis();
      enter(AD_CYCLE_WATCHING);
      return;

    case AD_CYCLE_WATCHING:
      if (g_hold) {
        g_t0 = now;
        return;
      } /* a transfer is in flight; do not close */
      /* TWO DEADLINES, AND THE FIRST ONE IS THE PEER'S. watch_ms bounds how long we look;
         AD_WAKE_SAFE_MS bounds how late a connect can still be accepted, measured from the
         instant the advertisement stopped. Only the second is physics: past it an iPhone
         refuses, so a connect attempted there fails having done everything else right --
         which is exactly how this failed, with an ECONNRESET at the last stage and nothing
         to say why. Whichever comes first ends the watch. */
      if ((uint32_t)(now - g_advert_stop_ms) >= AD_WAKE_SAFE_MS) {
        /* Closed by the peer's clock. If this line never appears and "closing/watch" always
           does, AD_WAKE_SAFE_MS is not the binding deadline and the watch is shorter than
           it needs to be. */
        cycle_log("closing/safe", (uint32_t)(now - g_watch_started_ms), "watched_ms",
                  (uint32_t)(now - g_advert_stop_ms), "since_advert");
        enter(AD_CYCLE_CLOSING);
        return;
      }
      if (!due) return;
      cycle_log("closing/watch", (uint32_t)(now - g_watch_started_ms), "watched_ms", (uint32_t)(now - g_advert_stop_ms),
                "since_advert");
      enter(AD_CYCLE_CLOSING);
      return;

    case AD_CYCLE_CLOSING:
      if (g_awdl_up) {
        awdl_end();
        g_awdl_up = false;
      }
      enter(AD_CYCLE_WAKING);
      g_t0 = now - g_cfg.advertise_ms; /* start the next advertisement immediately */
      return;
  }
}

int ad_cycle_phase(void) { return g_phase; }

uint32_t ad_cycle_phase_left_ms(void) {
  const uint32_t len = phase_len();
  const uint32_t el = (uint32_t)(millis() - g_t0);
  uint32_t left = (el >= len) ? 0 : (len - el);
  /* IN WATCHING, REPORT THE DEADLINE THAT ACTUALLY CLOSES IT. phase_len() is watch_ms, but
     the window now ends on AD_WAKE_SAFE_MS measured from the advertisement stopping, and
     that is usually the earlier of the two. Reporting the later one told a caller it had
     time it did not have -- and a caller uses this to decide whether a send can still
     finish its first connect. g_t0 also moves while a transfer holds the cycle open, which
     makes the watch_ms figure meaningless there; the advert-based one stays true. */
  if (g_phase == AD_CYCLE_WATCHING) {
    const uint32_t since = (uint32_t)(millis() - g_advert_stop_ms);
    const uint32_t safe = (since >= AD_WAKE_SAFE_MS) ? 0 : (AD_WAKE_SAFE_MS - since);
    if (safe < left) left = safe;
  }
  return left;
}

void ad_cycle_hold(bool on) { g_hold = on; }

void ad_cycle_end(void) {
  if (g_ble_up) {
    bleadv_enable(false);
    bleadv_end();
    g_ble_up = false;
  }
  if (g_awdl_up) {
    awdl_end();
    g_awdl_up = false;
  }
  g_phase = AD_CYCLE_OFF;
  g_hold = false;
}

#else
/* Not a sender build: no cycle, no advertiser, nothing linked. ad_cycle_begin() below is
   the only symbol a receiving sketch could reach, and it refuses. */
#include "ad_cycle.h"
const uint8_t AD_WAKE_ADV[24] = {0};
int ad_cycle_begin(const struct AdCycleCfg*) { return -1; }
int ad_cycle_last_error(void) { return -1; }
void ad_cycle_poll(void) {}
int ad_cycle_phase(void) { return AD_CYCLE_OFF; }
uint32_t ad_cycle_phase_left_ms(void) { return 0; }
void ad_cycle_hold(bool) {}
void ad_cycle_end(void) {}
#endif /* ESP32DROP_SENDER */
