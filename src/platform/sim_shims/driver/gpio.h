#pragma once

// Intentionally empty.
//
// main.cpp includes <driver/gpio.h> unconditionally, so the file has to
// resolve. Nothing in it is used: the only code that calls gpio_* is the X4
// battery-latch block, and soc/soc_caps.h compiles that out on the host.
