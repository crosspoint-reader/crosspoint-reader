# Xteink X3: reserve a damaged top band

The `x3_damaged_top` PlatformIO environment reserves the upper 13% of an X3
in portrait orientation. The physical band is painted black; CrossPoint lays
out its UI and books in the remaining `528 x 688` logical viewport. Because
the framebuffer is byte-packed, the requested percentage is rounded up and
byte-aligned: 13% becomes 104 of the panel's 792 physical rows (about 13.1%).

This is intentionally an opt-in build, so normal builds are unaffected.  It
also locks the device to portrait: rotating the UI would turn the physical
damaged top edge into a side edge and make a rectangular safe viewport invalid.
The renderer mechanism itself is panel-agnostic; the build environment scopes
it to the X3 by pairing the percentage with its 792-pixel panel width.

Build the firmware from a recursive clone:

```sh
pio run -e x3_damaged_top
```

The resulting image is:

```text
.pio/build/x3_damaged_top/firmware.bin
```

Flash it with CrossPoint's web installer by selecting X3 and **Custom .bin**,
or with a known-good serial connection:

```sh
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash \
  0x10000 .pio/build/x3_damaged_top/firmware.bin
```

Use a data-capable USB-C cable. Some third-party X3 units are USB locked; use
the Xteink Unlocker only when required by the device and only with its supported
firmware choices.

If CrossPoint is already installed, an SD card is also sufficient: copy the
image to the card, then select **Settings → System → SD Card Firmware Update**
and choose the `.bin` file. CrossPoint validates the ESP32 image before writing
the inactive OTA slot and restarts into it when complete.
