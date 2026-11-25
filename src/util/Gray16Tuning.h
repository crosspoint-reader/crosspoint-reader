#pragma once

#include <HalDisplay.h>

#if CROSSPOINT_GRAY16_TUNING
#include <Arduino.h>

// Serial commands for calibrating the panel's 16 gray levels. Each takes
// effect at once; nothing is saved. `l` is a level, 0 = white .. 15 = black.
//
//   CMD:GRAY16 DUMP              print the numbers for every level
//   CMD:GRAY16 ON                draw 16-level frames with these numbers
//   CMD:GRAY16 OFF               back to the built-in levels
//   CMD:GRAY16 SET l b p c       level l (1..15): b frames toward black from
//                                white, p frames back toward white, c frames
//                                toward white to clear it
//   CMD:GRAY16 SET 0 b w o       top-up for unchanged pixels: b black frames
//                                for one that stays black, w white frames for
//                                one that stays white. o = 1 draws new ink
//                                while old ink clears, 0 only after it
//   CMD:GRAY16 SHOW k            draw test pattern k as a fast frame
//   CMD:GRAY16 CLEAN k           draw it from white: a white frame, then the pattern
//
//   CMD:GRAY16 BARS S|C hex      draw 16 bars per half with the given levels,
//                                one hex digit each: the upper half's 16,
//                                then the lower half's. S = fast, C = clean
//
// Patterns (panel-native, 16 bars across the long side):
//   0  levels 0..15 above, 15..0 below      1  the two halves swapped
//   2  all white
// SHOW 0 then SHOW 1 changes every bar to another level, which shows what a
// gray-to-gray change looks like.
//
// Returns true when `command` (the text after "CMD:") was a GRAY16 command.
bool handleGray16Command(const String& command);
#endif
