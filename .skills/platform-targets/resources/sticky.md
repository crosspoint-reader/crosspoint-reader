---
device: sticky
device_flag: FREEINK_DEVICE_STICKY
sdk_profile: STICKY
sdk_header: freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h
shared_binary_envs: [sticky, sticky-gh_release, sticky-gh_release_rc]
mcu_family: s3
board_package: esp32-s3-devkitc1-n16r8
psram_in_ini: false
psram_on_silicon: true
fb_in_psram: false
sdmmc: false
block_device_interface: false
width: 800
height: 480
fb_bytes: 48000
controllers: [SSD1677]
grayscale: 4-level
viewable_insets: {top: 9, right: 3, bottom: 3, left: 3}
touch: GT911
frontlight: none
ui_scale: 1.2
ppi_note: ~220-235 PPI comment in BoardConfig; uiScale is hand-tuned
caps: [TOUCH, MIC, RTC, TEMP_HUMIDITY, IMU, BUZZER]
---

# Seeed Sticky

ESP32-S3. 3.97″ 800×480 SSD1677, GT911 (portrait digitizer, swap/flip in
profile). PDM mic, PCF8563, SHT40, LSM6DS3, BQ27220, LEDC buzzer. SPI SD
shares the panel bus. `uiScale` 1.2.

8 MB PSRAM is on the silicon; CrossPoint leaves it **off** (`BOARD_HAS_PSRAM`
is not set). Framebuffer stays in DRAM. S3 does not imply PSRAM is in play.

Own envs (`sticky`, `sticky-gh_release`, `sticky-gh_release_rc`). CI builds
`pio run -e sticky`. One S3 binary; not shared with X3/X4.
