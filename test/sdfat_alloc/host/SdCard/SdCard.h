// Host stand-in for SdFat's SdCard/SdCard.h: with USE_BLOCK_DEVICE_INTERFACE the
// volume only needs the block-device interface, not the SPI/SDIO card drivers.
#pragma once
// clang-format off: the interface header uses Sector_t without including SysCall.h.
#include <common/SysCall.h>
#include <common/FsBlockDeviceInterface.h>
// clang-format on
