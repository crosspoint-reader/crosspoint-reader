# EInkDisplay library

This is a focused low-level interaction library for the X4 display.

It's best paried with a higher-level GFX library to help with rendering shapes, text, and images.
Along with dealing with display orientation.

See the [SSD1677 guide](./doc/SSD1677_GUIDE.md) for details on the implemention.

## Usage

### Setup

```cpp
// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#define EPD_SCLK 8   // SPI Clock
#define EPD_MOSI 10  // SPI MOSI (Master Out Slave In)
#define EPD_CS 21    // Chip Select
#define EPD_DC 4     // Data/Command
#define EPD_RST 5    // Reset
#define EPD_BUSY 6   // Busy

EInkDisplay display(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY);
display.begin();
```

### Rendering black and white frames

```cpp
// First frame
display.clearScreen();
uint8_t* frameBuffer = einkDisplay.getFrameBuffer();
// ... your drawing code here, writing to frameBuffer, a 0 bit is black, a 1 bit is white ...
display.displayBuffer(FAST_REFRESH);

// Next frame
display.clearScreen();
uint8_t* frameBuffer = einkDisplay.getFrameBuffer();
// ... your drawing code here, writing to frameBuffer, a 0 bit is black, a 1 bit is white ...
// Using fash refresh for fast updates
display.displayBuffer(FAST_REFRESH);
```

### Rendering greyscale frames

```cpp
display.clearScreen();
uint8_t* frameBuffer = einkDisplay.getFrameBuffer();
// ... regular drawing code from before, all gray pixels should also be marked as black ...
display.displayBuffer(FAST_REFRESH);

 
// Grayscale rendering
// Refetch the frame buffer to ensure it's up to date
frameBuffer = einkDisplay.getFrameBuffer();

// Dark gray rendering
// Clear the screen with 0, all 0 bits are considered out of bounds for grayscale rendering
// Only mark the bits you want to be gray as 1
display.clearScreen(0x00);
// ... exact same screen content as before, but only mark the **dark** grays pixels with `1`, rest leave as `0` 
display.copyGrayscaleLsbBuffers(frameBuffer);

display.clearScreen(0x00);
// ... exact same screen content as before, but mark the **light and dark** grays pixels with `1`, rest leave as `0`
display.copyGrayscaleMsbBuffers(frameBuffer);
display.displayGrayBuffer();

// All done :)
```

### Power off

To ensure the display locks the image in, it's important to power off the display before exiting the program.

```cpp
display.deepSleep();
```
