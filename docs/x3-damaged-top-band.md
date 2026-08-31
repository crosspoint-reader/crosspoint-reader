# Xteink X3: reserve a damaged top band

The `x3_reserved_top` PlatformIO environment reserves a configurable percentage
of an X3 in portrait orientation. The physical band is painted black; CrossPoint
lays out its UI and books below it. There is no baked-in percentage: set
`CROSSPOINT_RESERVED_TOP_PERCENT` to an integer from 1 through 50 for each build.

For example, this reserves 13%:

```sh
CROSSPOINT_RESERVED_TOP_PERCENT=13 pio run -e x3_reserved_top
```

Because the framebuffer is byte-packed, the requested percentage is rounded up
and byte-aligned. On the X3, 13% becomes 104 of the panel's 792 physical rows
(about 13.1%) and leaves a `528 x 688` logical viewport.

This is intentionally an opt-in build, so normal builds are unaffected.  It
also locks the device to portrait: rotating the UI would turn the physical
damaged top edge into a side edge and make a rectangular safe viewport invalid.
The renderer mechanism itself is panel-agnostic; the build environment scopes
it to the X3 by pairing the percentage with its 792-pixel panel width.

The resulting image is:

```text
.pio/build/x3_reserved_top/firmware.bin
```

Flash it with CrossPoint's web installer by selecting X3 and **Custom .bin**,
or with a known-good serial connection:

```sh
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash \
  0x10000 .pio/build/x3_reserved_top/firmware.bin
```

Use a data-capable USB-C cable. Some third-party X3 units are USB locked; use
the Xteink Unlocker only when required by the device and only with its supported
firmware choices.

If CrossPoint is already installed, an SD card is also sufficient: copy the
image to the card, then select **Settings → System → SD Card Firmware Update**
and choose the `.bin` file. CrossPoint validates the ESP32 image before writing
the inactive OTA slot and restarts into it when complete.
