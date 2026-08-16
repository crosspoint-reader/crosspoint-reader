---
device: papermono
device_flag: FREEINK_DEVICE_PAPERMONO
sdk_profile: PAPER_MONO
sdk_header: freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h
shared_binary_envs: []
mcu_family: s3
board_package: esp32-s3-devkitc1-n16r8
psram_in_ini: false
psram_on_silicon: true
fb_in_psram: true
sdmmc: true
block_device_interface: false
width: 800
height: 480
fb_bytes: 48000
controllers: [SSD1677]
grayscale: 3-gray
viewable_insets: {top: 9, right: 7, bottom: 3, left: 7}
touch: FT5x06
frontlight: pmic-pwm
ui_scale: 1.0
ppi_note: null
caps: [TOUCH, FRONTLIGHT, MIC, RTC, BUZZER, LED]
---

# M5Stack Paper Mono

ESP32-S3. 800×480 **SSD1677** (BoardConfig; not SSD1683). FT6336 (FT5x06
family), PMIC frontlight (AW9967 via M5PM1), PDM mic, RX8130 RTC, buzzer,
discrete RGB LED, 1-bit SDMMC. `FREEINK_FB_PSRAM` defaults on. Bezel insets
9/7/3/7. Bring-up hooks live in `PaperMonoBoard.h` / `M5Ioe1.h`.

When a committed env sets this flag it is expected to include
`-DFREEINK_DEVICE_PAPERMONO=1`, `-DBOARD_HAS_PSRAM`, and
`-DUSE_BLOCK_DEVICE_INTERFACE=1`.
