---
device: x4pro
device_flag: FREEINK_DEVICE_X4PRO
sdk_profile: XTEINK_X4_PRO
sdk_header: freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h
shared_binary_envs: [x4pro, x4pro-gh_release, x4pro-gh_release_rc]
mcu_family: s3
board_package: esp32-s3-devkitc1-n16r8
psram_in_ini: true
psram_on_silicon: true
fb_in_psram: false
sdmmc: true
block_device_interface: true
width: 800
height: 480
fb_bytes: 48000
controllers: [SSD1677, UC8179, UC8279]
grayscale: 4-level
viewable_insets: {top: 9, right: 7, bottom: 3, left: 7}
touch: GT911
frontlight: pwm-warm
ui_scale: 1.2
ppi_note: touch-board uiScale 1.2
caps: [TOUCH, FRONTLIGHT, WARMLIGHT, RTC]
---

# Xteink X4 Pro

ESP32-S3, distinct from the C3 X4. 800×480; controller varies by batch
(SSD1677 / UC8179 / UC8279, probed at boot). GT911 with capacitive home key,
warm/cool PWM frontlight, 1-bit SDMMC, CW2017 gauge, PCF8563-compatible RTC.
`uiScale` 1.2. Bezel insets 9/7/3/7. 8 MB PSRAM on silicon.

Own envs (`x4pro` and release siblings). Those set `-DFREEINK_DEVICE_X4PRO=1`,
`-DBOARD_HAS_PSRAM`, `-DUSE_BLOCK_DEVICE_INTERFACE=1`, and
`-DFREEINK_FRONTLIGHT_LS`. CI builds `pio run -e x4pro`. Framebuffer stays
in DRAM; PSRAM is on for other heaps.
