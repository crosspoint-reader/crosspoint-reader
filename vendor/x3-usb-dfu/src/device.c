#include <string.h>

#include "softusb.h"

static SU_HOT bool event_room(const su_device* d) { return d->event_count < SU_EVENT_COUNT; }
static SU_HOT void push(su_device* d, su_event event) {
  d->events[(d->event_read + d->event_count) % SU_EVENT_COUNT] = event;
  d->event_count++;
}
bool su_next_event(su_device* d, su_event* event) {
  if (!d->event_count) return false;
  *event = d->events[d->event_read];
  d->event_read = (d->event_read + 1) % SU_EVENT_COUNT;
  d->event_count--;
  return true;
}
void su_device_init(su_device* d) { memset(d, 0, sizeof(*d)); }
void su_bus_reset(su_device* d) {
  su_device_init(d);
  su_event event = {0};
  event.type = SU_EVENT_RESET;
  push(d, event);
}
bool su_submit(su_device* d, uint8_t ep, uint8_t* buffer, uint16_t length) {
  if ((ep & 0x7f) != 0 || length > SU_MPS || (length && !buffer)) return false;
  su_endpoint* e = &d->ep[ep >> 7];
  if (e->queued || e->stalled) return false;
  e->buffer = buffer;
  e->length = (uint8_t)length;
  e->queued = true;
  if (ep) {
    e->packet = (su_packet){0};
    e->packet.pid = e->toggle ? SU_DATA1 : SU_DATA0;
    e->packet.len = (uint8_t)length;
    if (length) memcpy(e->packet.data, buffer, length);
  }
  return true;
}
bool su_set_address(su_device* d, uint8_t address) {
  if (address > 127 || !su_submit(d, 0x80, NULL, 0)) return false;
  d->pending_address = address;
  d->address_pending = true;
  return true;
}
void su_stall(su_device* d, uint8_t ep) {
  if ((ep & 0x7f) != 0) return;
  d->ep[ep >> 7].stalled = true;
  d->ep[ep >> 7].queued = false;
  if (ep) d->awaiting_ack = false;
}
static SU_HOT bool handshake(su_packet* r, uint8_t pid) {
  *r = (su_packet){0};
  r->pid = pid;
  return true;
}
void su_reject_packet(su_device* d) {
  d->token = 0;
  d->awaiting_ack = false;
}
bool su_receive(su_device* d, const su_packet* p, su_packet* r) {
  if (p->pid == SU_ACK) {
    if (d->awaiting_ack) {
      d->awaiting_ack = false;
      su_endpoint* e = &d->ep[1];
      // Event capacity is reserved before sending the IN packet.
      if (e->queued && event_room(d)) {
        e->queued = false;
        e->toggle = !e->toggle;
        su_event event = {0};
        event.type = SU_EVENT_XFER;
        event.ep = 0x80;
        event.len = e->length;
        push(d, event);
        if (d->address_pending) {
          d->address = d->pending_address;
          d->address_pending = false;
        }
      }
    }
    d->token = 0;
    return false;
  }
  // Any intervening non-ACK packet invalidates the previous IN's ACK window.
  // The queued IN packet remains unchanged so a host retry gets identical DATA.
  d->awaiting_ack = false;
  if (p->pid == SU_SOF || p->pid == SU_NAK || p->pid == SU_STALL) {
    d->token = 0;
    return false;
  }
  if (p->pid == SU_SETUP || p->pid == SU_OUT || p->pid == SU_IN) {
    d->token = 0;
    if ((p->token & 127) != d->address || (p->token >> 7) != 0) return false;
    if (p->pid != SU_IN) {
      d->token = p->pid;
      return false;
    }
    if (d->ep[1].stalled) return handshake(r, SU_STALL);
    if (!d->ep[1].queued || !event_room(d)) return handshake(r, SU_NAK);
    *r = d->ep[1].packet;
    d->awaiting_ack = true;
    return true;
  }
  uint8_t token = d->token;
  d->token = 0;
  if (p->pid != SU_DATA0 && p->pid != SU_DATA1) return false;
  if (p->len > SU_MPS) return false;
  if (token == SU_SETUP) {
    if (p->pid != SU_DATA0 || p->len != 8) return false;
    // A new SETUP aborts the prior control transfer and its unconsumed events.
    // Do not discard a bus-reset event that the stack has not consumed yet.
    bool reset = false;
    for (unsigned i = 0; i < d->event_count; i++)
      if (d->events[(d->event_read + i) % SU_EVENT_COUNT].type == SU_EVENT_RESET) reset = true;
    d->event_read = d->event_count = 0;
    if (reset) {
      su_event event = {0};
      event.type = SU_EVENT_RESET;
      push(d, event);
    }
    memset(d->ep, 0, sizeof(d->ep));
    d->ep[0].toggle = d->ep[1].toggle = true;
    d->address_pending = false;
    su_event event = {0};
    event.type = SU_EVENT_SETUP;
    memcpy(event.setup, p->data, 8);
    push(d, event);
    return handshake(r, SU_ACK);
  }
  if (token != SU_OUT) return false;
  su_endpoint* e = &d->ep[0];
  if (e->stalled) return handshake(r, SU_STALL);
  bool toggle = p->pid == SU_DATA1;
  if (toggle != e->toggle) {
    // Lost device ACK: acknowledge the already accepted packet without copying
    // again or reporting another completion, even if no next buffer is ready.
    return e->accepted ? handshake(r, SU_ACK) : false;
  }
  if (!e->queued || !event_room(d)) return handshake(r, SU_NAK);
  if (p->len > e->length) return false;  // Babble/overrun: never write past the buffer.
  if (p->len) memcpy(e->buffer, p->data, p->len);
  e->queued = false;
  e->toggle = !e->toggle;
  e->accepted = true;
  su_event event = {0};
  event.type = SU_EVENT_XFER;
  event.len = p->len;
  push(d, event);
  return handshake(r, SU_ACK);
}
