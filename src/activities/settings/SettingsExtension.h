#pragma once

#include <vector>

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
