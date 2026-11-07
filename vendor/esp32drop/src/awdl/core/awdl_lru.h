/* awdl_lru.h -- the one staleness comparison, shared by every MAC-keyed table.
 *
 * Three tables in this firmware are the same shape: a small fixed array of rows keyed by
 * a 6-byte MAC, each carrying the millis() of the last frame from it, evicting the
 * stalest row when a new key arrives and the array is full. The master table
 * (awdl_peer.h), the per-sender residual table (awdl_src.h) and the neighbour census
 * (awdl_census.h) each had their own copy of the scan.
 *
 * The scan itself stays duplicated on purpose: it runs per frame, over a different
 * element type each time, and an indirection through offsets or a callback would put a
 * multiply and a call in a path whose whole budget is measured in microseconds. What is
 * NOT duplicated any more is the part that was subtly wrong in all three copies at once
 * -- deciding which of two timestamps is older.
 *
 * ⚠️ THE KNOWN DEFECT LIVES HERE. This compares the timestamps directly rather than
 * comparing AGE against a common `now`, so it is not safe across the millis() wrap at
 * 49.7 days: immediately after a wrap the FRESHEST row holds the smallest value and is
 * evicted first -- which is exactly the row we are synced to. The firmware has never run
 * that long, so the behaviour is preserved rather than quietly changed during an
 * extraction; but a library gets left running for months, and when that is fixed this is
 * the single line to fix. tools/peer_test.c pins both the normal case and the defect.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Is a row last heard at `a` staler than one last heard at `b`? See the warning above. */
static inline bool awdl_lru_staler(uint32_t a, uint32_t b) { return a < b; }
