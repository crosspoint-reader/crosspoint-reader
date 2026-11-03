/* awdl_port_esp32.h -- the AWDL link layer's entry points on the ESP32 backend.
 *
 * AWDL takes the radio EXCLUSIVELY. It puts the interface in promiscuous mode, parks it on
 * channel 6, and transmits on a schedule locked to the mesh availability window. It cannot
 * coexist with WiFi.begin(), and that is not a limitation of this port: 100 % lock duty
 * requires hearing every frame on ch6.
 *
 * WHAT RUNS WHERE, because a user's task priorities can break this from outside:
 *   awdl_proc    core 1, prio 3   the frame path. Nothing on it may block.
 *   awdl_cad     core 1, prio 2   the transmit cadence. It owns presence, which is why
 *                                 your loop() may block for as long as it likes.
 *   loopTask     core 1, prio 1   yours.
 * A user task on core 1 at priority >= 3 starves the frame path; at >= 2 it starves the
 * cadence. AwdlDiag's ring_drop and AwdlStatus's win_served/win_total are how that becomes
 * visible instead of mysterious.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/awdl_diag.h"
#include "../core/awdl_peer.h"
#include "../core/awdl_peertab.h" /* struct AdPeerTab, for awdl_peertab_read() */
#include "../core/awdl_select.h"
#include "../core/awdl_src.h"
#include "../core/awdl_status.h"
#include "../core/awdl_tick.h"
#include "../core/awdl_window.h"
#include "esp_err.h"
#include "esp_netif.h"

/* --- lifecycle ------------------------------------------------------------------- */

/* Brings up the radio, the netif, the frame task and the cadence task, and derives this
 * device's AWDL identity from the interface MAC.
 *
 * Returns ESP_OK or the first failure. It does NOT abort: every step used to be wrapped in
 * ESP_ERROR_CHECK, which panics the chip, and a library may not decide that a user's boot
 * is over. A caller that ignores the return gets a device that is simply not on the air --
 * which awdl_status_read() will say plainly. */
esp_err_t awdl_begin(void);

/* Bring the link down and hand the memory back -- the inverse of awdl_begin(), and a
 * begin() after it works.
 *
 * WHY IT EXISTS. A device that both wakes a peer over BLE and then talks to it over AWDL
 * cannot hold both stacks at once and still leave anything for the application: measured
 * on an ESP32-S3, AWDL + the mbedTLS context + the BLE controller together leave 2,956 B
 * of internal RAM, while running them in turn leaves 25,424 B. Taking turns needs this.
 *
 * The two tasks are asked to leave and are waited for; they are never deleted from
 * outside, because the frame task takes a spinlock that no later begin() could release.
 * The radio goes down before the netif, so no frame arrives for an interface that is
 * already gone. Roughly 21.2 KB of that is the netif transmit queue alone -- sixteen slots
 * of MTU + headroom, 1,322 bytes each. It was 25.6 KB while a slot was a flat 1,600, and
 * shrinking the frame to the MTU is what returned the difference; a stale figure here would
 * have hidden the saving that change was made for.
 *
 * Returns ESP_OK, or ESP_ERR_INVALID_STATE if the link was not up or a task did not
 * acknowledge within one second -- and in that case NOTHING is freed and the link keeps
 * running. Reporting a teardown that did not happen is worse than not tearing down.
 *
 * NOT undone: nvs_flash_init(), esp_netif_init() and esp_event_loop_create_default().
 * Those are process-wide and shared; begin() already tolerates finding them present. */
esp_err_t awdl_end(void);

/* The transmit cadence normally owns its own task. This runs one pass by hand, for the
 * rollback build (-DAWDL_CADENCE_TASK=0) and for anyone who would rather drive it. */
void awdl_cadence_pass(void);

/* --- the RX tap: read every received IPv6 frame, beside the stack and after it ------
 *
 * Registered by the layer above -- AirDrop uses it to answer an mDNS query at the instant
 * it is heard, the only moment the querier is provably on channel 6. Deferring that to the
 * netif queue can cost a full 1,048,576 us cycle.
 *
 * IT RUNS ON THE FRAME TASK. Nothing in it may block: no Serial, no lwIP, no waiting. The
 * only legal side effects are awdl_diag_stage() and awdl_tx_ip6_now(), and the second is
 * enforced rather than trusted. process_frame INCLUDING the tap has one AW -- 16,384 us. */
typedef void (*awdl_rx_tap_fn)(void* ctx, const uint8_t src[6], const uint8_t* ip6, size_t iplen);
int awdl_rx_tap_set(awdl_rx_tap_fn fn, void* ctx);

/* Inject one complete IPv6 packet NOW, outside the window gate. The port adds the
 * 802.11 + LLC + awdl_data wrap.
 *
 * LEGAL ONLY from an RX tap or from the announce callback -- both run at a moment the
 * port has already established is on-channel. Anywhere else it refuses and increments
 * awdl_tx_now_refused(). Cost, measured and part of the contract: 3,400-7,379 us of
 * frame-path time, 20-45 % of one availability window. Do not send what you can skip. */
/* Bytes of link-layer header the port writes IN FRONT of your packet. Build your IPv6
 * packet at buf + AWDL_TX_HEADROOM and pass buf; the frame is assembled in place, with no
 * copy. Internal RAM is the binding resource here -- mbedTLS wants ~31.5 KB of it in one
 * contiguous piece and can allocate nothing else -- so the port does not keep a second
 * copy of a frame you already hold. */
#define AWDL_TX_HEADROOM 40
int awdl_tx_ip6_now(const uint8_t dst[6], uint8_t* buf, size_t len);
uint32_t awdl_tx_now_refused(void);
/* This surface keeps its OWN result, because AwdlDiag::tx_err is last-write-wins across
   every transmitter and the MIF cadence overwrites it four times a second. */
uint32_t awdl_tx_now_count(void);
uint32_t awdl_tx_now_errors(void);
int32_t awdl_tx_now_last_err(void);

/* The cadence knows WHEN to announce; the layer above knows WHAT an announcement is.
 * Fired from the cadence task, inside a window it has already decided to transmit in, so
 * awdl_tx_ip6_now() is legal for the duration of the call. */
int awdl_set_announce_cb(void (*fn)(void* ctx), void* ctx);

/* The only diagnostic call legal from the frame path: a memcpy into a drop-counted ring,
 * read back later by awdl_diag_read_line(). A header with a null payload is a plain
 * one-line message. */
bool awdl_diag_stage(const char* hdr, const void* p, int n);
bool awdl_diag_budget_ok(void); /* per-second capture budget, so a busy peer cannot
                                   flood the ring and evict the rare event */

/* --- identity -------------------------------------------------------------------- */

struct AwdlIdentity {
  uint8_t mac[6];    /* our AWDL interface MAC (locally administered)             */
  uint8_t ll[16];    /* fe80::EUI-64 of that MAC -- the address peers connect to  */
  char instance[16]; /* 12-hex service-instance and host label                    */
  char pair_sid[48]; /* UUID-shaped session id carried in the pairing TXT         */
};
void awdl_identity(struct AwdlIdentity* out);

/* --- reaching a peer we have only HEARD --------------------------------------------
 *
 * AWDL never runs NDP: a peer's IPv6 link-local is the deterministic EUI-64 of its AWDL
 * MAC. Verified equal on four devices [M] -- three during the receive work, and macOS's
 * own awdl0 (5a:51:ce:e9:4c:e8 -> fe80::5851:ceff:fee9:4ce8). That is what
 * makes a peer you merely HEARD into a peer you can address. */
void awdl_ll_of_mac(const uint8_t mac[6], uint8_t out_ll[16]);

/* Put a peer into lwIP's ND6 cache so an OUTBOUND connection to it can resolve.
 *
 * The receive path seeds a peer the moment it addresses US, which is all a receiver
 * needs and nothing a sender can use: the target has never sent us a unicast frame, so
 * ND6 holds nothing, lwIP falls back to real NDP, and AWDL never answers an NS.
 *
 * Idempotent, and internally rate-limited to one seed per peer per 500 ms. Call it
 * before connect() AND while the connection is open: lwIP's neighbour cache holds about
 * three entries (LWIP_ND6_NUM_NEIGHBORS), so a busy room evicts a peer mid-transfer and
 * the symptom is a stall, not an error. */
void awdl_seed_peer(const uint8_t mac[6]);

/* The lwIP interface index for sockaddr_in6::sin6_scope_id. A link-local socket without
 * a scope id goes nowhere, and neither connect() nor errno says why. 0 before begin(). */
int awdl_netif_index(void);

/* --- the transmit window --------------------------------------------------------- */

/* Microseconds until the window opens. 0 = it is open now. NEGATIVE = no lock, so there is
 * no window to wait for and no window to protect. Test for < 0 before comparing. */
int64_t awdl_window_next_us(void);

/* The published window, as the transmit gate itself sees it. */
void awdl_winstate_read(struct AwdlWinState* out);

esp_netif_t* awdl_netif(void);

/* --- telemetry: pull, never push -------------------------------------------------- */

void awdl_status_read(struct AwdlStatus* out);
void awdl_selection_read(struct Selection* out);
void awdl_tick_read(struct AwdlTick* out);

/* READ AND CLEAR, and the second half is not optional. proc_max_us and cb_max_us are
 * documented as "worst since the last read"; whoever prints them has always been the thing
 * that resets them. A read-only accessor would silently turn every one of them into a
 * since-boot maximum, which changes the meaning of every proc_max this project has ever
 * measured without producing an error anywhere. Pass false only if you are sampling
 * alongside another reader that does the clearing. */
void awdl_diag_read(struct AwdlDiag* out, bool clear_maxima);

/* The two MAC-keyed tables, copied under ONE critical section. They are 2,848 bytes
 * together, so hold this in static storage rather than on a task stack. It replaces seven
 * separate acquisitions of the same lock by the STAT line -- and that lock is taken on the
 * frame path, where a 223 us hold is already the measured worst case. */
struct AwdlSnapshot {
  struct AwdlPeerTable peers;
  struct AwdlSrcTable srcs;
};
void awdl_snapshot_read(struct AwdlSnapshot* out);

/* Nearby AWDL peers, with RSSI and whether each has been heard offering _airdrop._tcp.
 * This is the send path's input: RSSI is what tells a sample that somebody has come close
 * enough to mean it. `ttl_ms` drops rows not heard from in that long -- pass something
 * under the ~100 s AWDL MAC rotation, or the table will offer a MAC that no longer
 * answers; 0 disables expiry. See ESP32Drop/src/awdl/core/awdl_peertab.h. */
void awdl_peertab_read(struct AdPeerTab* out, uint32_t now_ms, uint32_t ttl_ms);

/* The mesh-wide phase gauge, summarised. The gauge object itself is 2,764 bytes and is
 * written continuously by the frame task; this is the dozen scalars a diagnostic prints. */
struct AwdlGaugeStatus {
  int locked;
  uint32_t c_off;
  int32_t c_rate, march, march_clamp;
  int health; /* frames of the last 32 that agree. 24-32 is healthy;
                 under 8 with awfix not advancing is the one alarm. */
  uint32_t n_awfix, n_suff, n_veto, n_sick, n_reacq, n_quar, n_alarm;
  int n_op, n_active;
  uint32_t rebase_total;
};
void awdl_gauge_status_read(struct AwdlGaugeStatus* out);
uint32_t awdl_gauge_rebase_of(const uint8_t mac[6]);

uint32_t awdl_cadence_stack_free_words(void);

/* --- staged diagnostics ----------------------------------------------------------- */

/* Bulk captures (mDNS payloads, AWDL service TLVs) staged from the frame path and
 * formatted here as one hex line. Same contract as ad_diag_read_line: copies ONE whole
 * line, returns its length, 0 when empty.
 *
 * These are the reason the frame path stays fast. Writing 958 bytes of hex directly from
 * process_frame cost 21 ms -- longer than one availability window -- and it was invisible
 * for a long time because the dump is capped per boot, so any capture taken minutes after
 * reset looked clean. Three of four "clean" datasets were exactly that. */
size_t awdl_diag_read_line(char* dst, size_t cap);
