#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "../../vendor/esp32drop/src/awdl/core/awdl_gauge.h"

static void check(int32_t d) {
  ga_t gauge;
  ga_reset(&gauge);
  uint16_t expected[GA_NB];
  unsigned shift = (((uint32_t)d) & GA_MASK) >> GA_BSH;
  for (unsigned i = 0; i < GA_NB; i++) {
    gauge.hist[i] = (uint16_t)(i * 7919u);
    expected[(i + shift) & (GA_NB - 1)] = gauge.hist[i];
  }
  ga_rotate(&gauge, d);
  assert(!memcmp(gauge.hist, expected, sizeof(expected)));
}
int main(void) {
  for (int shift = 0; shift < GA_NB; shift++) {
    check(shift * GA_BIN);
    check(-(int32_t)(shift * GA_BIN));
    check(shift * GA_BIN + GA_BIN - 1);
  }
  check(INT_MIN);
  check(INT_MAX);
  puts("Gauge rotation: all shifts and signed boundary offsets passed");
}
