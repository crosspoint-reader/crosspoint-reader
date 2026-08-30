#pragma once

#include <functional>
#include <vector>

#include "activities/ActivityResult.h"
#include "activities/settings/SettingsTypes.h"

// Optional hook for a downstream integration to contribute extra Settings
// tabs without CrossPoint knowing about the integration's specific settings,
// actions, or naming. Unset by default (nullptr), which reproduces
// CrossPoint's built-in category set exactly, with no extra branching.
//
// A plain function pointer keeps the no-provider case a single null check,
// with no heap allocation and no virtual dispatch. SettingsActivity calls the
// provider once per rebuild (screen re-entry, a setting change, a language
// switch) -- the same cadence it already uses to rescan fonts/dictionaries --
// so a provider whose categories change (e.g. after sign-in) stays current.
using SettingsExtensionProvider = std::vector<SettingsExtensionCategory> (*)();

void setSettingsExtensionProvider(SettingsExtensionProvider provider);
SettingsExtensionProvider getSettingsExtensionProvider();

// Runs `followUp` after the screen an extension action opened has returned --
// or immediately, when the action opened no screen.
//
// Activity::startActivityForResult() only *queues* the child and stores the
// caller's result handler in `installed`; nothing the child does has happened
// by the time the action handler returns. A host that refreshes its rows right
// there would therefore read pre-child state, leaving a row whose label depends
// on what the child changed stale until the next unrelated rebuild. Chaining
// onto the installed handler instead makes the refresh observe the child's
// effects, without the host having to know what any particular action does.
void runAfterExtensionAction(ActivityResultHandler& installed, std::function<void()> followUp);
