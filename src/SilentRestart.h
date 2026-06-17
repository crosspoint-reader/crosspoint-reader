#pragma once

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();          // home screen
void silentRestartToReader();  // currently-open book (APP_STATE.openEpubPath)

// One-shot heap-defrag retry for reader allocations that failed due to
// fragmentation. Returns false if the retry was already used in this boot path.
bool silentRestartToReaderForHeapRecovery();
void clearReaderHeapRecoveryRestart();
