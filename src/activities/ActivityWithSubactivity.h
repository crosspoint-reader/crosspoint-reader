#pragma once

#include "Activity.h"

// ActivityWithSubactivity is an alias for Activity.
// In the CJK fork, activities that manage sub-activities (e.g. FontSelectActivity)
// use this class. Currently it provides no additional functionality beyond Activity.
using ActivityWithSubactivity = Activity;
