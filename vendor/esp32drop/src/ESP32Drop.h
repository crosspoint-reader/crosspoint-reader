/* ESP32Drop.h -- AirDrop on the ESP32-S3: receive files from an iPhone or a Mac, or send a
 * file to one.
 *
 * Include this one header. It pulls in ESP32AWDL.h, because AirDrop over AWDL is not a
 * protocol you can run on ordinary Wi-Fi: the link layer underneath takes the radio
 * exclusively and cannot coexist with WiFi.begin(). If that is a problem for your
 * project, it is a problem you should discover here rather than three days in.
 *
 * Two roles, one per build. ad_begin(), ad_on_file() and ad_poll() receive;
 * ad_sender_begin(), ad_send() and ad_send_poll() send, and the sending modules compile only
 * when ESP32DROP_SENDER is defined -- in a build_opt.h beside the sketch, so the library
 * sees it too. A device cannot do both: a listener task's stack and a TLS client context
 * do not both fit beside AWDL. See examples/PrintReceivedFiles and examples/GreetingCard.
 *
 * The API is the C surface declared in the headers below. A C++ wrapper is planned on top
 * of it.
 */
#pragma once
#include "ESP32AWDL.h"

/* The dependency-free core: the codecs and the framing rules, each with a host suite. */
#include "airdrop/core/ad_dnssd.h"
#include "airdrop/core/ad_http.h"
#include "airdrop/core/ad_serve.h"
#include "airdrop/core/ad_status.h"
#include "airdrop/core/ad_zlib.h" /* the DvZip/zlib writer, used to BUILD an archive */

/* The ESP32 backend: TLS, the listener task, and the received-file handoff. */
#include "airdrop/port/ad_port_esp32.h"

/* The sending half. A device does whichever role it begins -- ad_begin() to receive,
 * ad_sender_begin() to send -- and the second one refused, because a listener task's stack
 * and a TLS client context do not both fit beside AWDL with anything left over.
 * ad_cycle.h is the BLE/AWDL alternation a sender needs; it contains no BLE code itself, so
 * a receiving sketch that never calls it carries no BLE dependency. */
#include "airdrop/ad_cycle.h"
#include "airdrop/ad_send.h"
