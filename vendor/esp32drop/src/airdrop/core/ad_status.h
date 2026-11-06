/* ad_status.h -- the AirDrop layer's answer to "how far did it get".
 *
 * The sibling of awdl_status.h, and it exists for the same reason: the layer below
 * already had a pull API, this one did not, so the sketch read twenty of its internal
 * globals directly. draw()'s own comment claimed the badge's screen "runs off the PUBLIC
 * status and nothing else" -- measured, six of its seven ladder rungs were reading
 * private AirDrop counters. This is that API.
 *
 * WHY COUNTERS AND NOT A BOOLEAN. "AirDrop doesn't work" is never one failure. A sender
 * has to find us over mDNS, open TCP, complete a TLS handshake, be answered on
 * /Discover, be answered on /Ask, and only then upload. Each stage is strictly harder to
 * reach than the one before it, so the LAST one that moved says where it stopped -- which
 * turns "it doesn't work" into "it stops after TLS", and that is a different bug report.
 *
 * Dependency-free C, so the same code compiles into the firmware and into a host test.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct AdStatus {
  /* --- the funnel. Each is cumulative; the last non-zero one is how far a sender got. */
  uint32_t tcp_accept;  /* TCP connections accepted on the AirDrop port          */
  uint32_t tls_ok;      /* ...of which completed a TLS handshake                 */
  uint32_t discover_ok; /* ...of which we answered POST /Discover (= the tile)   */
  uint32_t ask_ok;      /* ...of which we accepted POST /Ask                     */
  uint32_t upload_ok;   /* ...of which delivered a file                          */
  uint32_t desync;      /* streams abandoned because the framing was lost        */

  /* --- a transfer in flight ------------------------------------------------------- */
  bool rx_active;
  bool awaiting_upload;
  bool upload_pending;
  uint32_t socket_rx_bytes;
  uint32_t tls_failures;
  int last_tls_error;
  uint32_t rx_bytes; /* compressed bytes received so far                      */
  uint32_t rx_max;   /* the archive ceiling in force, in bytes. What ad_begin()
                        was asked for, or less when PSRAM could not supply it;
                        0 before the first transfer has sized it.             */

  /* --- files, and what went wrong ------------------------------------------------- */
  uint32_t files_ok;         /* files handed to the handler and accepted               */
  uint32_t file_err;         /* files the handler refused, plus archives we could not read */
  uint32_t limit_rejections; /* requests/uploads rejected at the configured size limit */
  char file_errmsg[40];

  /* --- discovery: before a sender can connect, it has to find us -------------------
   * The funnel starts here. A query we never answered is a tile that never appeared. */
  /* There is no "announcements sent" counter, and its absence is the point. AirDrop
     advertises through the AWDL service TLVs in the action frame; the unsolicited mDNS
     multicast announcement was removed because the radio will not transmit it and because
     every record it carried is served by answering a query. A counter that can only ever
     read zero is worse than no counter. */
  uint32_t query_seen;  /* mDNS queries that mentioned us or the service          */
  uint32_t resp_tx;     /* ...of which we answered                                */
  uint32_t notus;       /* ...asked for nothing we hold (parsed, so not answered) */
  uint32_t ka_suppress; /* ...already held every record we would have sent        */

  /* --- which advertised port a sender actually dials ------------------------------ */
  uint32_t tcp_tous;
  uint32_t syn_airdrop, syn_pair1, syn_pair2, syn_other;
  uint16_t syn_other_port;

  /* --- how our own TLS identity resolved at begin() -------------------------------- */
  const char* tls_alg;        /* "EC", "RSA", or "none" if the cert never loaded       */
  int ec_crt_err, ec_key_err; /* mbedTLS codes, if EC fell back to RSA        */
};
