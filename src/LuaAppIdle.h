#pragma once

// Called from Lua Host API blocking waits so power/sleep logic still runs while
// an app script holds the activity thread (e.g. World Clock while true loop).
void crosspointLuaIdleTick();
