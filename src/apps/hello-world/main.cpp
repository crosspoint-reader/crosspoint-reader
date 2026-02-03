#include <Arduino.h>
#include <HalDisplay.h>
#include <HalGPIO.h>

#include "HelloWorldActivity.h"

HalDisplay display;
HalGPIO gpio;
HelloWorldActivity activity(display, gpio);

void setup() {
  gpio.begin();

  // Only start serial if USB connected
  if (gpio.isUsbConnected()) {
    Serial.begin(115200);
    Serial.println("[HelloWorld] Starting...");
  }

  activity.onEnter();

  if (Serial) {
    Serial.println("[HelloWorld] Activity started");
  }
}

void loop() {
  gpio.update();
  activity.loop();
  delay(10);
}
