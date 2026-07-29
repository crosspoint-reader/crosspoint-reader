#pragma once

#include <builtinFonts/notoserif_12_bold.h>
#include <builtinFonts/notoserif_12_bolditalic.h>
#include <builtinFonts/notoserif_12_italic.h>
#include <builtinFonts/notoserif_12_regular.h>
#include <builtinFonts/notoserif_14_bold.h>
#include <builtinFonts/notoserif_14_bolditalic.h>
#include <builtinFonts/notoserif_14_italic.h>
#include <builtinFonts/notoserif_14_regular.h>
// 16/18pt reader cuts are physically enormous on the Murphy M3's 416x240 panel
// and the flash budget there is tight, so that build drops them (9/10pt cuts
// below replace them in the selectable set).
#if !FREEINK_DEVICE_MURPHY
#include <builtinFonts/notoserif_16_bold.h>
#include <builtinFonts/notoserif_16_bolditalic.h>
#include <builtinFonts/notoserif_16_italic.h>
#include <builtinFonts/notoserif_16_regular.h>
#include <builtinFonts/notoserif_18_bold.h>
#include <builtinFonts/notoserif_18_bolditalic.h>
#include <builtinFonts/notoserif_18_italic.h>
#include <builtinFonts/notoserif_18_regular.h>
#endif
#include <builtinFonts/notosans_8_regular.h>
#include <builtinFonts/notosans_12_bold.h>
#include <builtinFonts/notosans_12_bolditalic.h>
#include <builtinFonts/notosans_12_italic.h>
#include <builtinFonts/notosans_12_regular.h>
#include <builtinFonts/notosans_14_bold.h>
#include <builtinFonts/notosans_14_bolditalic.h>
#include <builtinFonts/notosans_14_italic.h>
#include <builtinFonts/notosans_14_regular.h>
#if !FREEINK_DEVICE_MURPHY  // see the notoserif 16/18 note above
#include <builtinFonts/notosans_16_bold.h>
#include <builtinFonts/notosans_16_bolditalic.h>
#include <builtinFonts/notosans_16_italic.h>
#include <builtinFonts/notosans_16_regular.h>
#include <builtinFonts/notosans_18_bold.h>
#include <builtinFonts/notosans_18_bolditalic.h>
#include <builtinFonts/notosans_18_italic.h>
#include <builtinFonts/notosans_18_regular.h>
#endif
#include <builtinFonts/ubuntu_10_bold.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_12_bold.h>
#include <builtinFonts/ubuntu_12_regular.h>

// Small-panel UI cuts (Murphy M3, 416x240 @ ~130 PPI): the standard 10/12pt UI
// faces render physically huge there, so the murphy build binds these under the
// same UI font IDs. Compiled in only for that device to keep other builds lean.
#if FREEINK_DEVICE_MURPHY
// Small-panel reader cuts: replace the dropped 16/18pt in the selectable set.
#include <builtinFonts/notosans_9_bold.h>
#include <builtinFonts/notosans_9_bolditalic.h>
#include <builtinFonts/notosans_9_italic.h>
#include <builtinFonts/notosans_9_regular.h>
#include <builtinFonts/notosans_10_bold.h>
#include <builtinFonts/notosans_10_bolditalic.h>
#include <builtinFonts/notosans_10_italic.h>
#include <builtinFonts/notosans_10_regular.h>
#include <builtinFonts/notoserif_9_bold.h>
#include <builtinFonts/notoserif_9_bolditalic.h>
#include <builtinFonts/notoserif_9_italic.h>
#include <builtinFonts/notoserif_9_regular.h>
#include <builtinFonts/notoserif_10_bold.h>
#include <builtinFonts/notoserif_10_bolditalic.h>
#include <builtinFonts/notoserif_10_italic.h>
#include <builtinFonts/notoserif_10_regular.h>
#include <builtinFonts/notosans_6_regular.h>
#include <builtinFonts/ubuntu_7_bold.h>
#include <builtinFonts/ubuntu_7_regular.h>
#include <builtinFonts/ubuntu_8_bold.h>
#include <builtinFonts/ubuntu_8_regular.h>
#endif
