#pragma once
#include <BatteryMonitor.h>

#define BAT_GPIO0 1  // Battery voltage

static BatteryMonitor battery(BAT_GPIO0);
