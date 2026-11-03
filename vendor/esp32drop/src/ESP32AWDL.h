/* ESP32AWDL.h -- the AWDL link layer: IPv6 to nearby Apple devices, with no access point,
 * no router and no pairing.
 *
 * THE RADIO IS TAKEN EXCLUSIVELY. AWDL parks the interface on channel 6, keeps it
 * promiscuous, and transmits on a schedule locked to the mesh availability window. It
 * cannot share the interface with WiFi.begin(), and that is inherent rather than an
 * oversight: holding 100 % lock duty requires hearing every frame on ch6.
 *
 * What the link is actually like, measured, so nothing here surprises you later: only
 * about 5 % of airtime is usable, so plan for a slow link; the interface MTU is 1280 --
 * the IPv6 minimum and the AWDL path MTU -- which puts the send MSS at 1220 and every
 * frame at 1320 bytes or under; inbound IPv6 fragments are not reassembled; and macOS
 * rotates its own AWDL MAC roughly every 100 seconds, so any design that pins a peer by
 * address will break.
 *
 * The hard ceiling above that MTU is the driver's, and it is silent. esp_wifi_80211_tx
 * takes raw frames of 24..1500 bytes and refuses anything longer with ESP_ERR_INVALID_ARG,
 * printing nothing (ieee80211_raw_frame_sanity_check; esp_wifi.h:1191). With the 40-byte
 * 802.11+LLC+awdl_data header this layer prepends (AWDL_TX_HEADROOM) that is an IPv6
 * packet ceiling of 1460 and a TCP payload ceiling of 1400; 1280 is the deliberate margin
 * under it. The same call also refuses a DATA frame whose addr1 is a group address, which
 * is why multicast leaves as an ACTION frame -- a separate rule, and just as silent.
 *
 * What is published today is the C surface in awdl/port/awdl_port_esp32.h. A C++ AWDL
 * singleton is planned on top of it.
 */
#pragma once

/* The public tier: what a user is meant to ask. */
#include "awdl/core/awdl_select.h"
#include "awdl/core/awdl_status.h"
#include "awdl/core/awdl_window.h"

/* The DIAGNOSTICS tier. Not the v1.0 API and not covered by its stability promise --
 * these are the raw instrument panel, published so that a diagnostics sketch can read
 * every counter the library keeps.
 * awdl_diag.h says it in its own header: "These are NOT the public API." */
#include "awdl/core/awdl_diag.h"
#include "awdl/core/awdl_elect.h"
#include "awdl/core/awdl_frame.h"
#include "awdl/core/awdl_gauge.h"
#include "awdl/core/awdl_lru.h"
#include "awdl/core/awdl_peer.h"
#include "awdl/core/awdl_peertab.h"
#include "awdl/core/awdl_ring.h"
#include "awdl/core/awdl_src.h"
#include "awdl/core/awdl_tick.h"

/* The ESP32 backend: bring-up, the window, and the pull-only telemetry. */
#include "awdl/port/awdl_port_esp32.h"
