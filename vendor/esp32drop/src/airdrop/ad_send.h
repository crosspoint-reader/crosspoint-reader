/* ad_send.h -- send a file to a nearby Apple device over AirDrop.
 *
 * The receiving half of this library answers AirDrop; this half initiates it. They do not
 * coexist on one device: a sender holds a TLS client context for its whole life and a
 * receiver holds a listener task with a 32 KB stack, and the two together do not fit
 * alongside AWDL with anything left for an application. Whichever role begins first owns
 * the device; the other returns AD_SEND_E_ROLE.
 *
 * WHAT A SEND ACTUALLY INVOLVES, because the API only makes sense against it:
 *
 *   1. The peer's AirDrop listener is not running until a Continuity BLE advertisement
 *      wakes it, and it closes again some time after that advertisement stops. How long
 *      either takes is not something this library claims to know -- see ad_cycle.h, which
 *      owns the alternation. This file assumes AWDL is up and the peer awake.
 *   2. /Discover, /Ask and /Upload each ride their OWN TLS connection, which is what a real
 *      macOS sender does. Three connects, three handshakes.
 *   3. /Ask makes the peer show its Accept dialog. A human then presses a button, or does
 *      not. Measured waits run from 4 s to 47 s, and "never" is a normal outcome.
 *
 * Point 3 is why this is asynchronous. Blocking for 47 s would freeze a sketch's screen and
 * buttons, and no Arduino user expects a call to do that. ad_send() returns immediately and
 * ad_send_poll() advances one step per call -- the same verb, on the same task, as the receiving
 * side. No thread is created: the largest contiguous internal block just after AWDL comes
 * up has been measured as low as 16,372 bytes, so a task stack here would come straight out
 * of the application's share.
 */
#ifndef AD_SEND_H
#define AD_SEND_H

#include <stdbool.h>
#include <stdint.h>

#include "../ESP32AWDL.h" /* struct AdPeerTab: produced by the AWDL layer, re-exported here */

/* ---- the role ---------------------------------------------------------------------- */

/* Claim the sender role and take the TLS client context: one contiguous block of about
 * 33 KB, held for the life of the program.
 *
 * ⚠️ CALL THIS FIRST, in setup(), BEFORE awdl_begin() and before any BLE. The order is
 * measured, not stylistic: taken after the radios have been up and down once, the same
 * allocation fails with ALLOC_FAILED while 39,560 bytes are still free, because none of it
 * is contiguous enough. mbedTLS wants two ~16.6 KB buffers side by side.
 *
 *   name: what the peer's Accept dialog calls you. UTF-8.
 *
 * Returns 0, or AD_SEND_E_*. */
int ad_sender_begin(const char* name);

/* ---- who is nearby ------------------------------------------------------------------ */

#define AD_PEER_TTL_MS 10000u

/* Copy the table of nearby AWDL peers in one critical section.
 *
 * Being in this table means a device is actively running AWDL right now -- AirDrop, AirPlay
 * or Handoff. Rows older than AD_PEER_TTL_MS are dropped on the way out: the table survives
 * awdl_end()/awdl_begin(), and an AWDL MAC rotates roughly every 100 s, so a stale row would
 * hand you an address that no longer answers. A row is a handle, not an identity.
 *
 * Choosing who to send to is yours. This returns rssi_avg and saw_airdrop; it does not pick.
 *
 * ⚠️ Only meaningful while ad_cycle_phase() == AD_CYCLE_WATCHING. In the BLE phase AWDL is
 * down and nothing is being heard. */
void ad_peers_read(struct AdPeerTab* out);

/* ---- sending ------------------------------------------------------------------------ */

/* Bytes of UTF-8, not characters -- 40 Japanese characters. The name is written three
 * times (FileName, FileBomPath "./name", and the cpio entry) and all three have to agree,
 * so it is bounded here, where refusing costs nothing, rather than truncated inside
 * build_ask() where it was silent: an 80-byte FileBomPath buffer cut the name at 77 bytes
 * while FileName and the archive went out whole. A bound on our own buffers, not a
 * measured peer limit. */
#define AD_SEND_NAME_MAX 120

/* Keeps ad_pack_bound() and adz_dvzip_record_bound() far from uint32 wrap, so an
 * uninitialised len is refused as an argument instead of allocating a wrapped size. Not a
 * peer limit either: the real ceiling is PSRAM, and ad_send() reports it as
 * AD_SEND_E_TOO_BIG. */
#define AD_SEND_LEN_MAX (64u * 1024 * 1024)

struct AdSendFile {
  const char* name;    /* what the peer shows and saves under. UTF-8, no '/', 1 to
                          AD_SEND_NAME_MAX bytes. Borrowed like data: it must stay valid
                          until the send ends. The extension is NOT read -- what the file
                          IS comes from `type` or from the bytes. macOS itself maps
                          ".bin" to com.apple.macbinary-archive, so a name-based guess
                          would declare "data.bin" a 1985 Mac archive.                 */
  const uint8_t* data; /* borrowed. Must stay valid and unchanged until the send ends.
                          The archive and its DvZip framing are built in PSRAM at about
                          2x len; no copy is made in internal RAM.                     */
  uint32_t len;        /* 1 to AD_SEND_LEN_MAX                                         */
  uint32_t mtime;      /* seconds since the Unix epoch, for the receiver's file date.
                          0 is accepted and lands the file on 1 Jan 1970 -- pass a real
                          time if the sketch has one (NTP, an RTC, a build stamp).     */
  const char* type;    /* WHAT THE FILE IS. A MIME type -- "image/png", "text/plain",
                          "application/pdf" -- or the Apple UTI the peer actually
                          receives in /Ask ("public.png") if you have it. Told apart by
                          the '/': a MIME type always has one, a UTI never does.
                          Borrowed, like name and data.
                          NULL means decide from the leading bytes, which works for
                          JPEG, PNG, GIF, HEIC, PDF and ZIP -- the kinds the receiver's
                          own sniffer knows -- and for nothing else. Anything the bytes
                          cannot name is refused with AD_SEND_E_TYPE rather than
                          labelled by guess, and so is a MIME type this library has no
                          UTI for, and so is a type the bytes provably contradict:
                          "image/png" over a JPEG does not go out.
                          The MIME-to-UTI table is core/ad_uti.h, read back from Apple's
                          own UniformTypeIdentifiers framework.
                          NOTE: public.png and public.jpeg are the values measured on
                          this wire, both to a Mac. Every other one is sent because you
                          asked for it, not because a peer is known to take it --
                          whether the Accept dialog appears for it, and where the file
                          lands afterwards, is unmeasured, and so is any type to an
                          iPhone. A peer that will not have it shows up as failed_in ==
                          AD_SEND_ASK, and the staged SEND-TYPE line says what was
                          declared.                                                    */
};

/* Stop after /Discover, before /Ask -- so before anything appears on anyone's screen.
 * Read airdropable and peer_name from the status, then call ad_send_continue() to go on or
 * ad_send_abort() to walk away leaving the peer none the wiser. A greeting-card sketch
 * wants this: whether a device answers /Discover at all is the only evidence that it can
 * receive, and it is not knowable before connecting. */
#define AD_SEND_HOLD 0x1u

/* THE LEAST WINDOW A SEND CAN START IN AND STILL MAKE ITS FIRST CONNECT.
 *
 * The connect step gives up after 8 s, so a send begun with less than that left will spend
 * its budget past the point where the peer still accepts one -- an iPhone stops at about 25
 * s after the wake advertisement stops. A send started too late does not fail faster; it
 * fails later, having taken the window's remaining time with it and snubbed a peer that was
 * never given a fair attempt. Compare it against ad_cycle_phase_left_ms() before calling
 * ad_send(). Two seconds of slack over the budget, for the TLS handshake to begin. */
#define AD_SEND_MIN_WINDOW_MS 10000u

/* Begin a send. Returns immediately; ad_send_poll() does the work. */
int ad_send(const struct AdPeerRow* peer, const struct AdSendFile* files, int nfiles, unsigned flags);
int ad_send_continue(void); /* legal only in AD_SEND_HOLD_ */
void ad_send_abort(void);   /* legal always */

/* Call every pass of loop(). Advances one step and returns.
 * While /Upload is running, how often you call this IS the transfer rate: a 200 ms draw()
 * costs speed, not correctness. */
void ad_send_poll(void);

/* Refusals from ad_send() itself -- what can be known before starting is said before
 * starting. */
enum {
  AD_SEND_E_OK = 0,
  AD_SEND_E_BUSY = -1,     /* a send is already running; one at a time                */
  AD_SEND_E_ROLE = -2,     /* this device began as a receiver                         */
  AD_SEND_E_NO_AWDL = -3,  /* awdl_begin() has not been called. NOT a lock check: a
                              send has succeeded on a cycle that read locked=0 just
                              beforehand, so requiring lock would refuse work that
                              would have completed.                                   */
  AD_SEND_E_NO_BEGIN = -4, /* ad_sender_begin() has not been called                   */
  AD_SEND_E_ARG = -5,      /* NULL, len 0 or over AD_SEND_LEN_MAX, nfiles != 1, or a
                              name that is empty, holds a '/', or is over
                              AD_SEND_NAME_MAX bytes.
                              nfiles is 1 because TotalBytes was measured equal to
                              FileSize for ONE file; its value for several is unknown. */
  AD_SEND_E_STATE = -6,
  AD_SEND_E_TYPE = -7,    /* what the file IS could not be settled, so nothing was
                             sent: type was NULL and the bytes are not one of the six
                             the sniffer knows; or a MIME type this library has no UTI
                             for; or a type the bytes provably are not. The fix is in
                             the sketch -- pass a MIME type, or the UTI itself. Never a
                             guess.                                                   */
  AD_SEND_E_TOO_BIG = -8, /* PSRAM could not hold the archive plus its framing, about
                             2x the file, at this moment. Refused before /Discover, so
                             no dialog appears for a file that cannot be built.       */
};

/* ---- how far it got ----------------------------------------------------------------- */

/* The stage. Each of DISCOVER, ASK and UPLOAD is a separate TLS connection. */
enum { AD_SEND_IDLE = 0, AD_SEND_DISCOVER, AD_SEND_HOLD_, AD_SEND_ASK, AD_SEND_UPLOAD, AD_SEND_DONE, AD_SEND_FAILED };
/* The step within it. With the stage, this says "stalled in the /Ask connection's
   handshake" rather than just "TLS". */
enum { AD_STEP_CONNECT = 0, AD_STEP_TLS, AD_STEP_REQUEST, AD_STEP_RESPONSE };

/* Named for what was observed, never for a mechanism that was not. */
enum {
  ADS_OK = 0,
  ADS_ERR_CONNECT_TIMEOUT, /* no answer to SYN. Usually the peer's listener had already
                              closed. How long a peer stays listening is not a number this
                              library claims to know.                                   */
  ADS_ERR_CONNECT_REFUSED, /* RST                                                      */
  ADS_ERR_TLS,             /* handshake failed; tls_err carries the mbedTLS code        */
  ADS_ERR_WRITE,
  ADS_ERR_RESPONSE_TIMEOUT, /* sent, nothing came back. In ASK this includes "the human
                               did nothing", which is a normal outcome                  */
  ADS_ERR_HTTP_STATUS,      /* answered, but not 2xx; http_status carries the number.
                               Whether a non-200 /Ask means Decline is NOT measured      */
  ADS_ERR_NOT_AIRDROPABLE,  /* /Discover answered but not 2xx. NOT the bplist IsAirDropable
                               field, which this version does not parse -- see
                               parse_discover() in ad_send.cpp for why                   */
  ADS_ERR_NO_MEM,           /* PSRAM could not hold about 2x len                        */
  ADS_ERR_ABORTED,
  ADS_ERR_PEER_CLOSED, /* the peer closed or reset the connection without answering
                          at all. NOT the same observation as a timeout, and it was
                          reported as one: measured against a device whose /Discover
                          had just succeeded, the /Ask connection was closed with zero
                          bytes returned in under three seconds, while the /Ask budget
                          is sixty. "Nothing came back yet" and "they hung up" call
                          for different next moves, and only one of them is worth
                          waiting through.                                          */
  ADS_ERR_STALLED,     /* a step stopped making progress and its budget ran out.
                          state and step say WHICH -- "stalled in the /Ask
                          connection's handshake" is a different fact from a
                          handshake that failed, and tls_err is 0 here because
                          mbedTLS never reported anything: it simply never finished.
                          Added because TLS and REQUEST had no budget at all, so a
                          peer that completed the TCP handshake and then went quiet
                          -- an iOS listener caught mid-shutdown, which ad_cycle.h
                          measures as 16 refusals in 17 probes between 25 and 30 s --
                          left the machine polling WANT_READ for ever, with
                          ad_cycle_hold(true) still set, so AWDL never came down and
                          no further BLE wake could happen.                        */
};

struct AdSendStatus {
  int state, step, err;
  int failed_in;      /* which STAGE was running when it failed -- AD_SEND_DISCOVER,
                         _ASK or _UPLOAD; 0 if it has not failed. state cannot carry
                         this, because it becomes AD_SEND_FAILED, and without it a
                         connect timeout is ambiguous: at DISCOVER the peer was never
                         listening, at UPLOAD it stopped listening while a human was
                         deciding. Those need opposite responses from a sketch.    */
  int http_status;    /* of the last response; 0 = none yet                       */
  int tls_err;        /* 0 = none                                                 */
  bool airdropable;   /* The peer answered /Discover with 2xx. Valid from
                         AD_SEND_HOLD_ on.
                         ⚠️ This is NOT the reply's IsAirDropable field: that value
                         sits over a kilobyte from its own key inside a bplist this
                         version has no reader for, and a scan wide enough to reach
                         it finds the wrong byte. 2xx is what the hardware-proven
                         path used as its only verdict.                            */
  char peer_name[64]; /* ALWAYS EMPTY in this version -- ReceiverComputerName lives
                         in the same unparsed bplist. Use peer_mac for a cooldown
                         key, and expect it to rotate about every 100 s.           */
  uint8_t peer_mac[6];
  uint32_t sent, total; /* /Upload body bytes                                       */
  int sock_errno;       /* why a connect gave up: the socket's errno, or 0 when it
                           simply never completed inside the budget                  */
  uint32_t elapsed_ms;
};

/* DONE and FAILED latch until the next ad_send(), so a slow polling loop cannot miss the
 * ending. */
void ad_send_status_read(struct AdSendStatus* out);

#endif /* AD_SEND_H */
