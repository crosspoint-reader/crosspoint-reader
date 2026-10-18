#include "reply_policy.h"

#include <string.h>
_Static_assert(offsetof(su_reply_policy, wave) == 516, "assembly wave offset");
_Static_assert(offsetof(su_reply_policy, response) == 756, "assembly response offset");
_Static_assert(offsetof(su_reply_policy, tx_cycle) == 760, "assembly TX timestamp offset");
_Static_assert(offsetof(su_reply_policy, in_code) == 764, "IN code offset");
_Static_assert(offsetof(su_reply_policy, in_wave) == 780, "IN wave offset");
_Static_assert(offsetof(su_reply_policy, host_ack) == 772, "ACK offset");
_Static_assert(offsetof(su_reply_policy, tx_end) == 776, "TX end offset");
void su_reply_policy_prepare(su_reply_policy* p, const su_device* d, unsigned output_offset) {
  // Only endpoint state changes per packet. The fixed handshake waveforms
  // are cached; unused IN storage need not be cleared while receiving OUT.
  static uint32_t cached_wave[3][20];
  static unsigned cached_offset;
  static bool wave_ready;
  if (!wave_ready || cached_offset != output_offset) {
    const uint8_t pids[] = {SU_ACK, SU_NAK, SU_STALL};
    for (unsigned i = 0; i < 3; i++) {
      su_packet packet = {0};
      packet.pid = pids[i];
      uint8_t symbols[SU_SYMBOL_MAX];
      unsigned n = su_encode(&packet, symbols);
      for (unsigned j = 0; j < 20; j++) cached_wave[i][j] = (j < n ? symbols[j] : SU_J) << output_offset;
    }
    cached_offset = output_offset;
    wave_ready = true;
  }
  p->address = d->address;
  memset(&p->response, 0, 6 * sizeof(uint32_t));
  memset(p->out, 0, sizeof(p->out));
  memset(p->setup, 0, sizeof(p->setup));
  p->setup[8] = SU_REPLY_ACK | SU_ACTION_SETUP;
  memcpy(p->wave, cached_wave, sizeof(cached_wave));
  const su_endpoint* e = &d->ep[0];
  unsigned expected = e->toggle ? 128 : 0, duplicate = e->toggle ? 0 : 128;
  if (e->stalled) {
    memset(p->out, SU_REPLY_STALL, SU_MPS + 1);
    memset(p->out + 128, SU_REPLY_STALL, SU_MPS + 1);
  } else {
    if (e->accepted) memset(p->out + duplicate, SU_REPLY_ACK | SU_ACTION_DUPLICATE, SU_MPS + 1);
    if (!e->queued || d->event_count == SU_EVENT_COUNT)
      memset(p->out + expected, SU_REPLY_NAK, SU_MPS + 1);
    else
      memset(p->out + expected, SU_REPLY_ACK | SU_ACTION_OUT, e->length + 1);
  }
  if (d->ep[1].stalled)
    p->in_code = SU_REPLY_STALL;
  else if (!d->ep[1].queued || d->event_count == SU_EVENT_COUNT)
    p->in_code = SU_REPLY_NAK;
  else {
    uint8_t symbols[SU_SYMBOL_MAX];
    p->in_code = 4;
    p->in_count = su_encode(&d->ep[1].packet, symbols);
    for (unsigned i = 0; i <= SU_SYMBOL_MAX; i++)
      p->in_wave[i] = (i < p->in_count ? symbols[i] : SU_J) << output_offset;
  }
}

void su_reply_policy_after_out(su_reply_policy* p, const su_device* d, unsigned output_offset) {
  const su_endpoint* e = &d->ep[0];
  if (p->address != d->address || p->in_code != SU_REPLY_NAK || d->ep[1].queued || d->ep[1].stalled || !e->queued ||
      e->stalled || !e->accepted || d->event_count == SU_EVENT_COUNT) {
    su_reply_policy_prepare(p, d, output_offset);
    return;
  }
  memset(&p->response, 0, 6 * sizeof(uint32_t));
  p->in_code = SU_REPLY_NAK;
  unsigned expected = e->toggle ? 128 : 0, duplicate = e->toggle ? 0 : 128;
  memset(p->out + expected, 0, SU_MPS + 1);
  memset(p->out + expected, SU_REPLY_ACK | SU_ACTION_OUT, e->length + 1);
  memset(p->out + duplicate, SU_REPLY_ACK | SU_ACTION_DUPLICATE, SU_MPS + 1);
}
