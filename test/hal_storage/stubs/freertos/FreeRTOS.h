// Host stand-in for FreeRTOS.h: the types the HAL's lock headers name.
#pragma once
#include <cstdint>

using TickType_t = uint32_t;
using BaseType_t = int;
#define portMAX_DELAY 0xFFFFFFFFu
