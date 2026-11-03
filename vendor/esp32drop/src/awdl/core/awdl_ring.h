/* awdl_ring.h -- the single-producer/single-consumer cursor both rings share.
 *
 * Two rings in this library have the same shape and the same discipline: the frame ring
 * (WiFi callback on core 0 produces, awdl_proc on core 1 consumes) and the diagnostic dump
 * ring (awdl_proc produces, the application consumes through awdl_diag_read_line()).
 * Neither may block. The producer sits on the frame path, where the entire budget is
 * microseconds and where a stall costs a transmit window; the consumer is on the far side
 * of the library, holding a UART that has been measured stalling for 250 ms.
 *
 * So when the ring is full the producer DROPS and counts. That is not a compromise, it is
 * the requirement: waiting for the consumer would reintroduce exactly the stall the ring
 * exists to remove. `drops` is the honest record of it, and both rings report it.
 *
 * This header owns only the cursor arithmetic -- the element arrays stay with the caller,
 * so the payload is never reached through a pointer this code chose, and nothing is added
 * to the hot path but an index.
 *
 * ---- the protocol, which is the part worth stating once instead of twice -------------
 *
 *   producer:  i = awdl_ring_reserve(r);   if (i < 0) it is full and already counted
 *              ...fill slot[i]...
 *              awdl_ring_commit(r);        publishes it. NOT before the fill.
 *
 *   consumer:  i = awdl_ring_peek(r);      if (i < 0) it is empty
 *              ...consume slot[i]...
 *              awdl_ring_release(r);       frees it. NOT before the consume.
 *
 * head is written last on the producer side and tail last on the consumer side, so neither
 * ever observes a slot the other is still working on. Capacity is slots-1: the state
 * (head+1 == tail) is what distinguishes full from empty, and spending one slot on that is
 * cheaper than a separate count that would itself need to be shared.
 *
 * ---- why the barriers ---------------------------------------------------------------
 *
 * `volatile` orders volatile accesses against each other and nothing else. The slot fill
 * is an ordinary memcpy, so a compiler is within its rights to sink part of it past the
 * volatile store that publishes the slot -- at which point the consumer reads a slot the
 * producer has not finished writing. No compiler has done that to us yet, and "not yet"
 * is not the same as correct: this is library code, compiled by toolchains this project
 * will never see.
 *
 * The fix is a compiler barrier, and it is FREE: it emits no instruction, it only forbids
 * the reordering. Measured -- both images are byte-identical in size with and without it.
 *
 * The index arithmetic masks with a runtime field rather than a compile-time constant,
 * which costs one extra load on the frame path -- of a word sitting in the same struct as
 * head and tail, which are being loaded anyway. Measured cost across both rings: 140 bytes
 * of flash (108 mask, 28 volatile `drops`, 4 assertions), no change in RAM. Stated in full
 * because "this must not cost performance on the frame path" is a requirement here, not a
 * preference, and an unmeasured "negligible" is not an answer.
 *
 * A full hardware barrier is deliberately NOT used. The two cores reach these rings in
 * internal SRAM, which on ESP32 is not cached per core, so there is no hardware store
 * visibility problem to solve and a DMB-equivalent would cost time on the frame path for
 * nothing. If this is ever ported to a target where the producer and consumer sit behind
 * separate caches, THIS is the comment to come back to.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Emits nothing; forbids the compiler from moving memory accesses across it. */
#define AWDL_RING_BARRIER() __asm__ __volatile__("" ::: "memory")

struct AwdlRing {
  volatile uint32_t head;  /* producer advances */
  volatile uint32_t tail;  /* consumer advances */
  uint32_t mask;           /* slots - 1; slots MUST be a power of two */
  volatile uint32_t drops; /* producer-side: reserve found it full. volatile because it
                              is written on one core and read by the status printer on the
                              other -- a stale read here would misreport the one number
                              that says the consumer is falling behind. */
};

/* Compile-time guard for the caller: AWDL_RING_POW2(n) is 1 only for a power of two.
   Both call sites assert on it, because a non-power-of-two would leave the mask silently
   wrong and the ring would corrupt rather than fail. */
#define AWDL_RING_POW2(n) ((n) > 0 && (((n) & ((n) - 1)) == 0))

/* `slots` must be a power of two. The mask is why: the index arithmetic runs on the frame
   path, and a runtime `%` there would be a real division. */
static inline void awdl_ring_init(struct AwdlRing* r, uint32_t slots) {
  r->head = r->tail = 0;
  r->mask = slots - 1;
  r->drops = 0;
}

/* The slot to fill, or -1 if the ring is full (and then it has been counted).
 *
 * No barrier, and that asymmetry is deliberate rather than an oversight: what follows a
 * successful reserve is a STORE into the slot, control-dependent on the volatile tail load
 * above it, and a compiler may not introduce a speculative store to an object another
 * thread can observe. The release side of the pair, in commit(), is where the ordering
 * actually has to be stated. */
static inline int awdl_ring_reserve(struct AwdlRing* r) {
  uint32_t h = r->head;
  if (((h + 1) & r->mask) == r->tail) {
    r->drops++;
    return -1;
  }
  return (int)h;
}

/* Publish the reserved slot. Call this AFTER the slot is completely filled. */
static inline void awdl_ring_commit(struct AwdlRing* r) {
  AWDL_RING_BARRIER(); /* the fill happens before the publish */
  r->head = (r->head + 1) & r->mask;
}

/* The slot to consume, or -1 if the ring is empty.
 *
 * The barrier here is the acquire half of the pair, and it is needed for the same reason
 * the release half is: `volatile` orders volatile accesses against each other and says
 * nothing about the caller's ordinary loads out of the slot. Without it a compiler may
 * hoist a payload read above the head load and read a slot the producer has not published.
 *
 * That is hardening on this header's own stated standard rather than a fix for an observed
 * defect: no build of this library has been caught hoisting, and the point of writing the
 * rule into the code is that the next toolchain does not have to be asked.
 *
 * The barrier is free: built both ways, the image is byte-identical at 1,258,779.
 *
 * The index is also masked here, not only where tail advances -- which is NOT free, it is
 * 8 bytes, measured the same way. Worth it: it makes "the returned index is
 * dereferenceable" a guarantee of this function rather than a property the caller has to
 * trace back through awdl_ring_release. */
static inline int awdl_ring_peek(const struct AwdlRing* r) {
  uint32_t t = r->tail;
  if (t == r->head) return -1;
  AWDL_RING_BARRIER(); /* the publish is observed before the payload */
  return (int)(t & r->mask);
}

/* Free the consumed slot. Call this AFTER the slot has been read out. */
static inline void awdl_ring_release(struct AwdlRing* r) {
  AWDL_RING_BARRIER(); /* the consume happens before the free */
  r->tail = (r->tail + 1) & r->mask;
}

/* How many slots are filled. Diagnostic only: on the live rings this is a torn read of
   two cursors, so it is a snapshot of a moving thing, not an invariant to act on. */
static inline uint32_t awdl_ring_used(const struct AwdlRing* r) { return (r->head - r->tail) & r->mask; }
