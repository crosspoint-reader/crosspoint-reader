#pragma once

// Claiming EXT1 wake support compiles out the X4 battery-latch block. That
// block is the only user of gpio_num_t / GPIO_NUM_13 / gpio_set_direction, so
// excluding it is what keeps driver/gpio.h empty.
#ifndef SOC_PM_SUPPORT_EXT1_WAKEUP
#define SOC_PM_SUPPORT_EXT1_WAKEUP 1
#endif
