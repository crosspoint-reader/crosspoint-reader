/* ad_cycle.h -- the BLE/AWDL alternation a sender has to perform, and the measured
 * constants that make it work.
 *
 * A sender cannot simply come up and transmit. Two facts force a cycle:
 *
 *   1. The peer's AirDrop listener is not running until a BLE Continuity advertisement
 *      wakes it. That the advertisement is what does it was established by elimination:
 *      unicast mDNS queries for _airdrop._tcp were fired at three peers every six seconds
 *      for seven minutes and not one listener appeared, and this badge had been naming
 *      _airdrop._tcp in its own AWDL service TLV for hours without waking anybody.
 *
 *   2. BLE and AWDL cannot both be resident. The BLE controller costs about 26 KB of
 *      internal DRAM and AWDL about 110 KB, and a device that holds both has nothing left
 *      for the application -- which is the whole objection to a protocol library that
 *      consumes the device.
 *
 * So: advertise, tear BLE down and release it, bring AWDL up, watch for someone close,
 * send, tear AWDL down, repeat. This module owns that loop so a sketch does not have to
 * rediscover the four timings or hand-assemble the 24-byte advertisement -- a single
 * missing length byte in that payload produced four cycles of silent failure, with
 * bleadv_begin() returning success the whole time.
 *
 * ⚠️ THIS WHOLE MODULE IS A SENDER'S. It compiles only under ESP32DROP_SENDER, along with
 * the bundled BLE advertiser it drives. A receiving build carries neither -- not the code,
 * not the controller, and not the btInUse() override that would otherwise keep about 36 KB
 * reserved for a radio that build never turns on.
 */
#ifndef AD_CYCLE_H
#define AD_CYCLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- the measured constants -------------------------------------------------------- */

/* The Continuity advertisement that wakes an Apple device's AirDrop listener.
 *
 * Pass it to your BLE driver verbatim. The first byte is the AD element's own length and
 * is NOT optional: a driver whose API takes the payload "without the leading length byte"
 * means the HCI command's significant-length field, not this one. Without it a scanner
 * reads an AD element of length 0xFF and discards the whole advertisement.
 *
 * The eight identifier bytes are SYNTHETIC. They are not derived from anyone's Apple ID,
 * contact details or hardware, and they identify no person. */
extern const uint8_t AD_WAKE_ADV[24];

/* HOW LONG TO ADVERTISE.
 *
 * There is no constant here for "how long the peer takes to wake", and that is deliberate:
 * the figure this file used to carry was never measured against the advertisement starting,
 * so quoting it at all invited it to be built on again.
 *
 * What is observed: one second is enough. At the 30 ms interval both captured Apple devices
 * use, a second is about thirty advertising events. Transfers complete through /Discover,
 * /Ask, a human Accept and /Upload on peers woken this way, and when a peer does NOT wake
 * the reason has not been duration -- an iPhone with its screen locked does not wake however
 * long the advertisement runs, and with the screen on it is ready at once. One capture holds
 * both outcomes against the same peer, one cycle apart, with the same advertisement.
 *
 * Being wrong here is cheap and loud: a peer that needed longer simply does not answer, and
 * that reads as "too late - they slept". Being wrong the other way is expensive and silent
 * -- BLE and AWDL cannot both be resident, so every second spent advertising is a second
 * the badge cannot see anybody at all. */
#define AD_WAKE_ADVERTISE_MS 1000u

#define AD_CYCLE_LOCK_MS                                        \
  12000u /* AWDL up -> phase lock, typical. A CAP, not a wait:  \
            the cycle leaves LOCKING as soon as the estimator   \
            reports locked, because lock is a settling delay    \
            and not a gate -- a send has succeeded on a cycle   \
            that read locked=0 immediately beforehand. Spending \
            it in full cost most of the iOS budget for nothing. */

/* HOW LATE A CONNECT CAN STILL BE ACCEPTED, measured from the instant the advertisement
 * STOPPED -- which is the only instant any of this is measured from on the peer's side.
 *
 * The two platforms hold their listener open for very different times after the advert
 * stops. Measured with a bare-connect probe every two seconds across a whole window:
 *
 *     macOS   open for the whole 40 s window -- 134 probes, not one refusal
 *     iOS     open to about 25 s, then refused: 25-30 s went 1 open / 16 refused
 *
 * Distance had nothing to do with it: 1.6 dB between the average RSSI of an accepted and
 * a refused probe. It is purely elapsed time.
 *
 * ⚠️ THIS USED TO BE COUNTED FROM THE WRONG INSTANT, and the arithmetic that justified it
 * was wrong in a way that made the code look safe. The reasoning was "20 s keeps every
 * attempt inside both, with 5 s of margin under the iOS cliff" -- true only if the watch
 * begins when the advertisement stops. It does not: LOCKING sits in between, and it was a
 * fixed 12 s. So connects were attempted between advert_stop + 12 s and advert_stop + 32 s,
 * the last seven seconds of every single cycle were past the point where an iPhone refuses,
 * and the stated margin did not exist anywhere in the window. The failure it produced is
 * the worst kind: a send that did everything right and died on ECONNRESET at the last
 * connect, with a phone sitting in front of the device.
 *
 * 20 s from advert stop keeps the author's intended 5 s of margin, now under the number it
 * was always meant to be under. LOCKING is adaptive as well (see AD_CYCLE_LOCK_MS), so the
 * settling time is no longer spent whole and the usable watch is longer than it was even
 * while being safer. */
#define AD_WAKE_SAFE_MS 20000u

/* HOW LONG TO LOOK, as a secondary cap. AD_WAKE_SAFE_MS is the deadline that matters; this
 * bounds a watch that would otherwise sit idle inside it, and is the knob a caller can
 * raise when every target is a Mac. Whichever expires first ends the watch. */
#define AD_CYCLE_WATCH_MS 20000u

/* ⚠️ WHAT THE WINDOW BOUNDS, AND WHAT IT DOES NOT.
 *
 * It bounds CONNECTS, not transfers. An established connection outlives it comfortably: a
 * measured send to an iPhone took 28 seconds from the first connect to the last byte and
 * completed, and a connection has been observed alive 47 seconds in. So a large file is not
 * what breaks a send.
 *
 * What must all land inside the window are the three connects -- /Discover, /Ask and
 * /Upload each open their own -- and the human's Accept sits between the second and the
 * third. Measured accepts run from 4 to 47 seconds. A person who takes twenty seconds
 * pushes the /Upload connect past the point where an iPhone still accepts one, and the send
 * fails with ECONNRESET at the last stage having done everything else right.
 *
 * That is the real cost of three connections, and it is why they are worth revisiting if
 * iOS ever turns out to accept a reused one. */

/* The BLE controller costs about 26 KB of internal DRAM when it really starts
 * (26,452 B measured standalone, 26,620 B in this tree). Half of
 * that is the floor ad_cycle_poll() requires before it will believe the radio is up --
 * comfortably below a real controller, comfortably above measurement noise. */
#define AD_BLE_MIN_BYTES 13000u

/* ad_cycle_last_error() when the advertiser reported success but took no memory, so it
 * cannot be advertising. Distinct from a negative BLE return code, which is the driver
 * admitting the failure itself. */
#define AD_CYCLE_E_BLE_SILENT (-1000)

/* ---- the loop's shape --------------------------------------------------------------- */

struct AdCycleCfg {
  uint32_t advertise_ms; /* 0 -> AD_WAKE_ADVERTISE_MS */
  uint32_t lock_ms;      /* 0 -> AD_CYCLE_LOCK_MS                                     */
  uint32_t watch_ms;     /* 0 -> AD_CYCLE_WATCH_MS. How long to look for someone close
                            before tearing AWDL down. Raising it past AD_WAKE_SAFE_MS has
                            no effect against an iPhone: that deadline is measured from
                            the advertisement stopping and closes the watch first. Raise
                            BOTH only if every target is a Mac. */
};

/* ---- the loop ----------------------------------------------------------------------- */

enum {
  AD_CYCLE_OFF = 0,
  AD_CYCLE_WAKING,   /* BLE advertising. AWDL is DOWN -- no peer can be seen yet.     */
  AD_CYCLE_LOCKING,  /* AWDL up, waiting for phase lock.                              */
  AD_CYCLE_WATCHING, /* the only phase in which ad_peers_read() means anything.        */
  AD_CYCLE_CLOSING,  /* AWDL coming down.                                             */
};

/* Start cycling. cfg may be NULL for the measured defaults. Returns 0, or -1 in a build
 * without ESP32DROP_SENDER, where there is no advertiser to drive. */
int ad_cycle_begin(const struct AdCycleCfg* cfg);

/* Call every pass of loop(). Never blocks and never delays: every wait is a millis()
 * comparison, because a sketch that sleeps through the 30-second BLE phase cannot draw a
 * screen or read a button, and this cycle spends roughly half its period there. */
void ad_cycle_poll(void);

int ad_cycle_phase(void);

/* Milliseconds remaining in the current phase, for a progress display. */
uint32_t ad_cycle_phase_left_ms(void);

/* Keep AWDL up past the watch window because a transfer is in flight. Set it when a send
 * starts and clear it when the send finishes -- without it the cycle would tear the radio
 * out from under an Accept dialog the human is still looking at. */
void ad_cycle_hold(bool on);

/* Stop cycling and release whatever is up. */
void ad_cycle_end(void);

/* The last failure from the radios this module drives, or 0 while all is well: a negative
 * BLE result, or an esp_err_t from awdl_begin(). It is kept because a wake that silently
 * fails to start is indistinguishable from a peer that is simply not there -- the sender
 * waits, nothing happens, and no counter says why. */
int ad_cycle_last_error(void);

#endif /* AD_CYCLE_H */
