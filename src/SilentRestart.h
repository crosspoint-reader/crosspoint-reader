#pragma once

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session — WiFi/LWIP/netif
// teardown scatters long-lived allocations across the heap, leaving ~50KB of
// contiguous space unrecoverable without a reboot.

void silentRestart();          // home screen
void silentRestartToReader();  // currently-open EPUB (APP_STATE.openEpubPath)
