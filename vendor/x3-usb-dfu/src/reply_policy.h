#ifndef SU_REPLY_POLICY_H
#define SU_REPLY_POLICY_H
#include "softusb.h"
#ifdef __cplusplus
extern "C" {
#endif
// Low two bits select no reply/ACK/NAK/STALL. Upper bits record deferred action.
enum {
  SU_REPLY_ACK = 1,
  SU_REPLY_NAK = 2,
  SU_REPLY_STALL = 3,
  SU_ACTION_OUT = 4,
  SU_ACTION_SETUP = 8,
  SU_ACTION_DUPLICATE = 12
};
typedef struct {
  uint32_t address;
  uint8_t out[256], setup[256];
  uint32_t wave[3][20];
  uint32_t response, tx_cycle;
  uint32_t in_code, in_count, host_ack, tx_end, in_wave[SU_SYMBOL_MAX + 1];
} su_reply_policy;
// Build a frozen EP0 snapshot. Serialize endpoint changes until commit; the
// output offset must be the verified dedicated-GPIO offset (0..6).
SU_HOT void su_reply_policy_prepare(su_reply_policy*, const su_device*, unsigned output_offset);
// Refresh a previously prepared snapshot after an ordinary OUT transaction.
// Falls back to a full preparation if another endpoint or address changed.
SU_HOT void su_reply_policy_after_out(su_reply_policy*, const su_device*, unsigned output_offset);
// first/second must be parsed, CRC/EOP-validated packets. NULL second means
// timeout or invalid packet. Call after every bounded exchange, before refreshing
// the snapshot. False means snapshot mismatch; stop servicing the bus.
SU_HOT bool su_reply_policy_commit(su_device*, const su_reply_policy*, const su_packet* first, const su_packet* second);
#ifdef __cplusplus
}
#endif
#endif
