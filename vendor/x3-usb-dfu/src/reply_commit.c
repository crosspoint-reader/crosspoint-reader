#include <string.h>

#include "reply_policy.h"

static unsigned code(const su_packet* p) {
  return p->pid == SU_ACK ? SU_REPLY_ACK : p->pid == SU_NAK ? SU_REPLY_NAK : p->pid == SU_STALL ? SU_REPLY_STALL : 20;
}
// Apply only complete, CRC/EOP-validated packets supplied by the physical layer.
// Work on a copy first: a stale/mismatched snapshot must not change the device
// or write its OUT buffer. The caller serializes snapshots, RX/TX, and commit.
bool su_reply_policy_commit(su_device* d, const su_reply_policy* p, const su_packet* first, const su_packet* second) {
  // The common OUT path needs no speculative whole-device copy. Validate the
  // frozen decision completely before mutating state or the caller's buffer.
  if (first && second && first->pid == SU_OUT && (second->pid == SU_DATA0 || second->pid == SU_DATA1) &&
      second->len <= SU_MPS) {
    su_endpoint* e = &d->ep[0];
    unsigned decision = 0;
    if ((first->token & 127) == d->address && (first->token >> 7) == 0) {
      if (e->stalled)
        decision = SU_REPLY_STALL;
      else if ((second->pid == SU_DATA1) != e->toggle) {
        if (e->accepted) decision = SU_REPLY_ACK | SU_ACTION_DUPLICATE;
      } else if (!e->queued || d->event_count == SU_EVENT_COUNT)
        decision = SU_REPLY_NAK;
      else if (second->len <= e->length)
        decision = SU_REPLY_ACK | SU_ACTION_OUT;
    }
    if (decision != p->response || p->host_ack) return false;
    if (decision == (SU_REPLY_ACK | SU_ACTION_OUT) && second->len && !e->buffer) return false;
    d->token = 0;
    d->awaiting_ack = false;
    if (decision == (SU_REPLY_ACK | SU_ACTION_OUT)) {
      if (second->len) memcpy(e->buffer, second->data, second->len);
      e->queued = false;
      e->toggle = !e->toggle;
      e->accepted = true;
      su_event event = {0};
      event.type = SU_EVENT_XFER;
      event.len = second->len;
      d->events[(d->event_read + d->event_count) % SU_EVENT_COUNT] = event;
      d->event_count++;
    }
    return true;
  }
  su_device next = *d;
  uint8_t scratch[SU_MPS] = {0};
  uint8_t* out = d->ep[0].buffer;
  next.ep[0].buffer = scratch;
  su_packet reply = {0};
  unsigned decision = 0;
  bool ack = false;
  if (!first)
    su_reject_packet(&next);
  else {
    bool have = su_receive(&next, first, &reply);
    if (have) decision = code(&reply);
    if (first->pid == SU_IN) {
      if (decision == 20) {
        // Verify that the queued packet still matches the retained waveform.
        uint8_t symbols[SU_SYMBOL_MAX];
        unsigned n = su_encode(&reply, symbols);
        if (n != p->in_count) return false;
        uint32_t k = p->in_wave[0];
        if (!k || (k & (k - 1))) return false;
        for (unsigned i = 0; i < n; i++)
          if (p->in_wave[i] != symbols[i] * k) return false;
        ack = second && second->pid == SU_ACK;
        if (ack)
          su_receive(&next, second, &reply);
        else
          su_reject_packet(&next);  // ACK window expired; retain queued DATA.
      }
    } else if (first->pid == SU_OUT || first->pid == SU_SETUP) {
      if (second && su_receive(&next, second, &reply)) {
        decision = code(&reply);
        if (decision == SU_REPLY_ACK)
          decision |= first->pid == SU_SETUP                 ? SU_ACTION_SETUP
                      : next.ep[0].toggle != d->ep[0].toggle ? SU_ACTION_OUT
                                                             : SU_ACTION_DUPLICATE;
      } else if (!second)
        su_reject_packet(&next);
    }
  }
  if (decision != p->response || ack != !!p->host_ack) return false;
  if (decision == (SU_REPLY_ACK | SU_ACTION_OUT) && second->len) {
    if (!out) return false;
    memcpy(out, scratch, second->len);
  }
  if (next.ep[0].buffer == scratch) next.ep[0].buffer = out;
  *d = next;
  return true;
}
