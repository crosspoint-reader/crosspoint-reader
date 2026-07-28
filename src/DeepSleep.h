#pragma once

// Starts deep sleep immediately unless the current activity defers it for
// required pre-sleep work.
void enterDeepSleep(bool fromTimeout = false);

// Completes a sleep request previously deferred by Activity::prepareForSleep().
void completeDeferredDeepSleep(bool fromTimeout);
