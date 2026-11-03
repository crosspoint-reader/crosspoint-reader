/* ad_port_esp32.h -- the AirDrop layer's entry points on the ESP32 backend.
 *
 * Everything here runs over a link the AWDL layer owns. There is no configuration for
 * that link and none is offered: AirDrop is not a protocol you can point somewhere else.
 *
 * THE CALLBACK RULE. ad_poll() is the only thing that runs user code, and it runs it on
 * the CALLER's task. Nothing in this header can put your code on the TLS task or the
 * frame task, which is why there is no "onReceive fires from wherever" option -- the
 * invariants those tasks carry are not ones a library can ask a user to honour.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/ad_ask.h"
#include "../core/ad_cpio.h"  /* AD_FILE_* and the archive walker */
#include "../core/ad_dnssd.h" /* AIRDROP_PORT */
#include "../core/ad_status.h"

/* One received file, valid ONLY for the duration of the callback. The bytes live in a
   PSRAM block that ad_poll() releases once the whole archive has been handed over -- copy
   what you need, do not keep the pointer.

   ONE CALLBACK PER FILE. A transfer is a cpio archive and may hold several; you get each
   one. The library does not choose between them, and in particular it does not filter by
   type -- `type` is sniffed from the leading bytes as a convenience, and AD_FILE_OTHER is
   delivered exactly like the rest. (The code this replaces returned the FIRST JPEG or PNG
   and discarded the archive, so a PDF, a text file, or the second of two photos vanished
   and the transfer was reported as "no displayable image".) */
struct AdFile {
  const uint8_t* data;
  uint32_t len;
  const char* name; /* basename, NUL-terminated                                */
  const char* path; /* as the sender wrote it, e.g. "./photo.png"              */
  int type;         /* AD_FILE_* -- from the bytes, never from the name        */
  uint32_t index;   /* 0-based position among the files in THIS transfer       */
  uint32_t count;   /* how many files this transfer carries                    */
};

/* Return false if you could not use the file. That is counted and reported through
   AdStatus.file_err / file_errmsg, so a badge that receives a format it cannot draw says
   so in its diagnostics instead of dropping the transfer silently. */
typedef bool (*ad_file_cb_t)(void* ctx, const struct AdFile* f);

void ad_on_file(ad_file_cb_t cb, void* ctx);

// Optional bounded DvZip archive stream, delivered only by ad_poll(). Register
// before ad_begin; callbacks must not navigate or stop the listener. Data is borrowed
// until callback return. Finish must validate the archive; Abort cleans partial output.
// In this mode max_receive limits decoded bytes, independently of available PSRAM.
enum class AdStreamEvent { Begin, Data, Finish, Abort };
typedef bool (*ad_stream_cb_t)(void*, AdStreamEvent, const uint8_t*, uint32_t);
void ad_on_stream(ad_stream_cb_t cb, void* ctx);

/* Starts the TLS listener. AWDL must already be up: this binds to the link-local address
 * the AWDL netif owns.
 *
 * `name` is what appears on the sender's share sheet. It is UTF-8, so "リビングのバッジ"
 * and "badge 🐱" work -- the plist carries non-ASCII as UTF-16BE. It is measured, not
 * assumed: the tile reads ReceiverComputerName out of the /Discover response, established
 * by giving three candidate fields three different values and looking.
 *
 * Returns 0; -1 if the listener task could not be created; -2 if the name could not be
 * encoded (malformed UTF-8, or longer than the format's one-byte length). A name that
 * cannot be encoded is refused rather than truncated: it is something a user typed. */
/* Without ad_on_stream(), `max_receive` bounds the DECODED archive this device will accept, in bytes, and with it
 * the PSRAM the library takes while a transfer is running. Pass 0 for the library's own
 * default of 6 MiB (RX_MAX_AUTO in ad_port_esp32.cpp). PSRAM only ever lowers that ceiling,
 * never raises it.
 *
 * WHY IT IS YOURS TO SET. Decoding happens while the body arrives, so the library holds
 * one block about the size of the finished archive for the length of a transfer. Left to
 * itself it takes nearly all of PSRAM -- which is right for a badge that does nothing else
 * and wrong for a sketch that wants a framebuffer, a sound sample or a log of its own.
 * Whoever wrote the sketch knows which; the library does not.
 *
 * A transfer larger than this is refused with HTTP 413 to the sender, so the Mac says the
 * transfer failed instead of appearing to succeed. That is a policy limit and it is
 * reported as one; running out of memory below the limit is a different fact and answers
 * 507. ad_stats_read() publishes the effective ceiling, which may be lower than you asked
 * for if PSRAM could not supply it.
 *
 * Sizes worth knowing, all measured: a 2.4 MB photo decodes to a 2,354,688-byte archive;
 * the largest free PSRAM block on an ESP32-S3R8 with this firmware is 7,864,308 B, and once
 * the residue buffer and RX_OUT_HEADROOM are taken out there is room for about 7.1 MB of
 * decoded archive. That is the most an explicit max_receive can actually get. */
int ad_begin(const char* name, uint32_t max_receive);

/* Fires pending callbacks on the calling task. Cheap when there is nothing to do.
   Blocking inside a callback costs nothing on the air: the transmit cadence is owned by
   the AWDL layer's own task and does not depend on this being called promptly. */
void ad_poll(void);

/* True once ad_begin() has claimed this device for receiving. The sender refuses to start
 * on a device that has, and vice versa: the two cannot share the memory. */
bool ad_role_is_receiver(void);

void ad_stats_read(struct AdStatus* out);

/* The SMALLEST free stack the TLS listener task has ever had, in bytes, or 0 before it
 * runs. The listener has a 32,768-byte PSRAM stack; mbedTLS heap buffers still use
 * internal RAM. Measure stack headroom on the target during a real transfer.
 *
 * That measurement is taken only under -DAD_DIAG_DETAIL: sampling it costs a
 * uxTaskGetStackHighWaterMark() on every pass of a 100 ms loop, for ever, to re-derive a
 * figure that is already known. Without the define this returns 0, the same as it does
 * before the task has run. */
uint32_t ad_listen_stack_min_free(void);

/* --- diagnostics ----------------------------------------------------------------
 *
 * The library never writes to Serial. It stages one line at a time into a small ring and
 * you decide where they go -- which is the only arrangement that works, because these
 * lines are produced on the TLS task and a user-supplied sink would run there too. There
 * is no setLogSink() for that reason.
 *
 * Copies ONE whole line (NUL-terminated, keeping its own trailing newline) into dst and
 * returns its length, or 0 when the ring is empty. Call it in a loop:
 *
 *     char line[200];
 *     while (ad_diag_read_line(line, sizeof(line))) Serial.print(line);
 *
 * Lines are dropped rather than blocking a protocol task when the ring is full;
 * ad_diag_dropped() is the count, and a non-zero value means you are draining too slowly,
 * not that anything is wrong on the air. */
size_t ad_diag_read_line(char* dst, size_t cap);
uint32_t ad_diag_dropped(void);

#ifdef AIRDROP_BODY_KEEP
/* --- raw-body probe (NOT part of the library's interface) ------------------------
 *
 * Only under -DAIRDROP_BODY_KEEP, which tools/build-bodykeep.sh sets. In that build the
 * /Upload body is NOT parsed: it is held so the sketch can write it out verbatim, because
 * a captured stream answers every question about the format at once and offline, while a
 * device-side instrument answers one question per physical AirDrop.
 *
 * ad_body_peek() returns the length and lends the pointer -- the library still owns the
 * memory. Call ad_body_release() when finished with it; nothing else frees it, so a
 * caller that forgets holds 2.35 MB of PSRAM and drops the next transfer. */
uint32_t ad_body_peek(const uint8_t** p);
void ad_body_release(void);
#endif

// CrossPoint lifecycle and explicit receive consent (callbacks remain on ad_poll).
// Prepare before AWDL consumes internal heap; release with ad_end even if startup fails.
bool ad_prepare_tls(void);
bool ad_end(void);
bool ad_upload_pending(void);
bool ad_request_pending(void);
bool ad_request_answer(bool accept);

// Snapshot of the pending consent metadata, copied under a short critical section.
bool ad_request_info(AdAskInfo* out);

// Called only from a stream callback when an archive header exceeds the limit.
void ad_reject_too_large();
