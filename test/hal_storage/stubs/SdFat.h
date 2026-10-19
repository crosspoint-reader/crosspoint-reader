// Host stand-in for SdFat.h: the FsLib part HalStorage.cpp uses (FsFile, FsVolume, FsDateTime),
// from the patched SdFat the sdfat_alloc test builds. The real header also pulls the card
// drivers, which the host has no use for.
#pragma once
#include <FsLib/FsLib.h>
