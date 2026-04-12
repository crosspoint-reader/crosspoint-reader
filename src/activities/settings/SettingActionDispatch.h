#pragma once
#include <memory>

#include "SettingInfo.h"

class Activity;
class GfxRenderer;
class MappedInputManager;

// Creates the sub-activity corresponding to the given SettingAction.
// Returns nullptr for None, Submenu, or unknown actions (caller handles those).
std::unique_ptr<Activity> createActivityForAction(SettingAction action, GfxRenderer& renderer,
                                                  MappedInputManager& mappedInput);
