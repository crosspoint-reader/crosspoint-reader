#include "Arduino.h"
#include "SPI.h"
#include "Wire.h"
#include "WiFi.h"

HWCDC Serial;
ESPClass ESP;
WiFiClass WiFi;
SPIClass SPI;
TwoWire Wire;

uint32_t esp_get_free_heap_size() { return ESP.getFreeHeap(); }
