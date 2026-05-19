#pragma once

// MCU is fixed across all supported hardware targets.
#define CROSSPOINT_MCU "esp32-c3"

// Returns the HTTP User-Agent string for this device, e.g.:
//   "CrossPoint/1.3.0 (X4; esp32-c3; 480x800)"
// Built once on first call using runtime GPIO and display state; safe to call
// after hardware init. CROSSPOINT_VERSION is injected by platformio.ini.
const char* getCrossPointHttpUserAgent();
