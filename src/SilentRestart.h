#pragma once

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();          // home screen
void silentRestartToReader();  // currently-open EPUB (APP_STATE.openEpubPath)
// True when this boot itself came from a silent restart. Callers that restart
// as a last-resort defrag must check this so a failure that survives the
// restart degrades to an error instead of a reboot loop.
bool bootWasSilentRestart();
