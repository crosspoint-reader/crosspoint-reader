// AWDL owns Wi-Fi exclusively on channel 6. Stop ordinary Wi-Fi before startup.
// Frame processing runs on core 1 at priority 3, cadence at priority 2.
// Neither task may block on display, storage, Serial, or application callbacks.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/awdl_peer.h"
#include "../core/awdl_select.h"
#include "../core/awdl_src.h"
#include "../core/awdl_tick.h"
#include "../core/awdl_window.h"
#include "esp_err.h"
#include "esp_netif.h"

// Starts the radio, netif and workers. Returns the first failure without aborting.
// Startup failure may leave partial resources; the caller must perform recovery.
esp_err_t awdl_begin(void);

// Joins workers, stops Wi-Fi and releases session buffers. Returns an error if
// teardown cannot complete; the caller must retain callback state and recover.
// Shared NVS, netif initialization and the default event loop remain initialized.
esp_err_t awdl_end(void);

// Manual cadence entry point for AWDL_CADENCE_TASK=0 builds.
void awdl_cadence_pass(void);

// Called on the frame task after stack delivery. Must not block. A discovery
// response can be injected immediately while the sender is known to be listening.
typedef void (*awdl_rx_tap_fn)(void* ctx, const uint8_t src[6], const uint8_t* ip6, size_t iplen);
int awdl_rx_tap_set(awdl_rx_tap_fn fn, void* ctx);

// Put the IPv6 payload at buf + AWDL_TX_HEADROOM. The link header is built in
// place. Allowed only from the RX tap or announce callback; len excludes headroom.
#define AWDL_TX_HEADROOM 40
int awdl_tx_ip6_now(const uint8_t dst[6], uint8_t* buf, size_t len);

// Called by cadence inside an eligible transmit window. Must not block.
int awdl_set_announce_cb(void (*fn)(void* ctx), void* ctx);

struct AwdlIdentity {
  uint8_t mac[6];     // locally administered AWDL MAC
  uint8_t ll[16];     // fe80::EUI-64 address
  char instance[16];  // service-instance and host label
  char pair_sid[48];  // pairing TXT session identifier
};
void awdl_identity(struct AwdlIdentity* out);
void awdl_ll_of_mac(const uint8_t mac[6], uint8_t out_ll[16]);

// Seeds a peer's deterministic IPv6-to-MAC mapping without NDP; rate limited.
void awdl_seed_peer(const uint8_t mac[6]);
// IPv6 socket scope ID; zero before startup.
int awdl_netif_index(void);
uint32_t awdl_cadence_stack_free_words(void);

// Main-task diagnostic snapshot; never used to make transport decisions.
struct AwdlTransportStats {
  uint32_t ringDrops;
  uint32_t acksSkipped, txQueueDrops;
  uint16_t channelMask;
  uint8_t queued, inflight;
  bool locked;
};
void awdl_transport_stats(AwdlTransportStats* out);
