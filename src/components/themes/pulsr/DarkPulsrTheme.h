#pragma once
#include "PulsrTheme.h"

// DarkPulsrTheme — inverted PULSR: black content area, white chrome bars.
// All drawing logic is shared with PulsrTheme; the inverted_ flag in the
// base class flips all color parameters at render time.
class DarkPulsrTheme : public PulsrTheme {
 public:
  DarkPulsrTheme() : PulsrTheme(/*inverted=*/true) {}
};
