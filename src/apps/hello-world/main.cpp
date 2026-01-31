#include <Arduino.h>
#include <EInkDisplay.h>
#include <InputManager.h>
#include "HelloWorldActivity.h"

// Display SPI pins for Xteink X4
#define EPD_SCLK 8
#define EPD_MOSI 10
#define EPD_CS 21
#define EPD_DC 4
#define EPD_RST 5
#define EPD_BUSY 6

EInkDisplay display(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY);
InputManager input;
HelloWorldActivity activity(display, input);

void setup() {
  Serial.begin(115200);
  Serial.println("[HelloWorld] Starting...");
  
  input.begin();
  activity.onEnter();
  
  Serial.println("[HelloWorld] Activity started");
}

void loop() {
  input.update();
  activity.loop();
  delay(10);
}
