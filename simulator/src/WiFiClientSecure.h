#pragma once
#include "NetworkClientSecure.h"

// On ESP32 WiFiClientSecure derives from NetworkClientSecure.
// In the simulator NetworkClientSecure already provides setInsecure().
using WiFiClientSecure = NetworkClientSecure;
