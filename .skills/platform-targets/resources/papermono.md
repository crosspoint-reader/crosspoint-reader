---
device: papermono
device_flag: FREEINK_DEVICE_PAPERMONO
sdk_profile: PAPER_MONO
sdk_header: freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h
shared_binary_envs: [papermono, papermono-gh_release, papermono-gh_release_rc]
mcu_family: s3
board_package: esp32-s3-devkitc1-n16r8
psram_in_ini: true
psram_on_silicon: true
fb_in_psram: true
sdmmc: true
block_device_interface: true
width: 800
height: 480
fb_bytes: 48000
controllers: [SSD1677]
grayscale: 3-gray
viewable_insets: {top: 9, right: 7, bottom: 3, left: 7}
touch: FT5x06
multitouch: false
has_home_key: false
frontlight: pmic-pwm
ui_scale: 1.0
ppi_note: null
usb_detect: none
shared_pads:
  13: sdmmc-clk
caps: [TOUCH, FRONTLIGHT, MIC, RTC, BUZZER, LED]
---

# M5Stack Paper Mono

Nobody has a released unit in hand today. YAML is **current firmware on
this checkout**, not shipping hardware. M5Stack's product page (and the
schematic PDF that page serves) is the vendor source — generally
authoritative for the published pin map and sheet. Do not rewrite
firmware to match it until a released unit is measured.

This file is the full **PaperMono** (NFC + LoRa SKU on the docs), not
PaperMono-Lite.

## Sources

1. **Current firmware (this checkout).** `BoardConfig::PAPER_MONO`,
   `PaperMonoDriver`, `PaperMonoBoard.h` / `M5Ioe1.h` / `M5Pm1.h`,
   committed `platformio.ini` `[env:papermono*]`, CI `pio run -e papermono`.
   In-tree SDK notes: `freeink-sdk/docs/display-driver-references.md`
   (primary: "M5 board implementation and on-glass measurements").
2. **First firmware cut.** `629512f` `feat: add Paper Mono hardware
   support` (MarsTechHAN); merge `032e7f6`. `DisplayController::SSD1683`,
   `Ssd1683Driver` (PaperToDo-compatible four-gray). Panel-native 800×480,
   FT6336 portrait 480×800 swapped in, keys GPIO2/3, 4-bit SDMMC
   `{13,12,11,10,9,8,4}`, `ImuType::None`, `NO_FRONTLIGHT` / `NO_MIC` /
   `NO_LEDS`.
3. **Later firmware.** `e165846` `PaperMonoBoard` / `M5Ioe1.h` (PMIC
   frontlight, discrete RGB, PDM mic, GPIO42 beeper, IOE1 probe).
   `71040ea` replaced the waveform with host-authored 3-gray LUTs from
   on-glass characterisation (files still named `Ssd1683Driver`; header
   already said SSD1677). `41f2a7f` renamed those files to
   `PaperMonoDriver` and set `DisplayController::SSD1677`.
4. **M5Stack docs** (vendor; generally authoritative for the published
   pin map / sheet). Product page:
   https://docs.m5stack.com/en/core/PaperMono
   Schematic V0.6.2 is a **sublink of that page**, not a separate origin:
   https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1267/PaperMono_SCH_V0.6.2_20260522.pdf
   (same file also as page-preview PNGs under `/1267/`). Size/weight on
   the page still WIP. The GitHub discussion attachment
   (`user-attachments/files/31221223/…`) is a rehost of this PDF, first
   seen via
   https://github.com/crosspoint-reader/crosspoint-reader/discussions/3099#discussioncomment-18079275

## Current firmware (this checkout)

ESP32-S3, 8 MB PSRAM (`board` `esp32-s3-devkitc1-n16r8`). 800×480
**SSD1677**, `PaperMonoDriver` 3-gray host LUTs. FT6336 as `Ft5x06` on
GPIO47/48, INT GPIO4, `ROTATE_180`. Keys GPIO2/3; power button is M5PM1
(`PaperMonoBoard::pollPowerButtonClicked`). AW9967 frontlight from M5PM1
PWM0 on the EPD rail. PDM GPIO45/46, rail IOE1 IO12. RX8130 0x32.
`ImuType::None`. Beeper GPIO42. RGB: PM1 `LED_EN` + IOE1 IO8/IO9. SDMMC
`busWidth` **4**. `usbDetect` unassigned. IOE1 `begin()` probes **0x6F then
0x4F**. `FREEINK_FB_PSRAM` on. Insets 9/7/3/7 (starting). Envs set
`-DFREEINK_DEVICE_PAPERMONO=1`, `-DBOARD_HAS_PSRAM`,
`-DUSE_BLOCK_DEVICE_INTERFACE=1`. CI: `pio run -e papermono`.

## Does the SDK align with m5-docs?

**Pins / peripherals: mostly yes, from the first profile.** EPD SPI
GPIO14/15/16/17/18, touch 47/48/4 @ 0x38, keys 2/3, SD CLK/CMD/DAT0–3,
RX8130 0x32, PM1 0x6E, buzzer 42, PDM 45/46 match the m5-docs pin map
(and the schematic PDF that page serves). That is not the same as
"copied from m5-docs": the first cut named the controller **SSD1683**,
which current m5-docs do not.

**Controller / gray: not the product page, not the first cut.** The SDK's
own table (`display-driver-references.md`) names **on-glass measurements**
as the primary driver source. `PaperMonoDriver` is SSD1677 + 3-gray host
LUTs. m5-docs say SSD1677 + **4-level**. The name now matches the page;
the waveform does not. CrossPoint `platformio.ini` comments still say
**SSD1683** and **1-bit SDMMC** (first-cut leftovers). The SDK sample env
comment says SSD1677 but still says **1-bit SDMMC**; `PAPER_MONO.sdmmc`
is 4-bit.

**IOE1 address: official page vs bring-up.** m5-docs list M5IOE1 **0x4F**
(the V0.6.2 schematic on that page). `M5Ioe1.h` says the official
schematic says 0x4F, the mono bring-up saw **0x6F**, so firmware probes
both. Register map is official `m5stack/M5IOE1`.

**IMU / LoRa / NFC: on the vendor page and its schematic, unused in the
profile.** Debug I2C scan comments expect 0x68 BMI270 and 0x50 NFC. Caps
stay off.

## M5Stack docs (vendor; page + schematic sublink)

Product page: ESP32-S3R8, 16 MB flash, 8 MB PSRAM, **3.97"** SSD1677
**480×800** **4-level**, FT6336G, frontlight, BMI270 0x68, RX8130CE
0x32, ST25R3916 0x50, Stamp LoRa-1262 868–923 MHz, M5PM1 0x6E, M5IOE1
**0x4F**, 4-bit microSD, 1150 mAh, two keys + power. Pin map matches the
firmware pads above.

Schematic V0.6.2 (same docs family): ESP32-S3-R8. Function map says EINK
(no SSD part on that map), touch-to-wake, KEY1/KEY2 + PWR_BTN, RX8130CE,
BMI270, SX1262, ST25R3916, AW9967, TF, RGB, PDM, buzzer. 4-bit TF
DATA0–3. USB D−/D+ GPIO19/20. L0–L3B via M5PM1 / M5IOE1. GPIO20 on the
sheet is USB D+, not VBUS detect.

## Divergences (firmware vs vendor docs; do not rewrite firmware)

| Topic | First firmware source | Current firmware | m5-docs (page + SCH V0.6.2) |
| --- | --- | --- | --- |
| Controller name | SSD1683 (`629512f`) | SSD1677 (`41f2a7f`, `PaperMonoDriver`) | page SSD1677; sheet "EINK" only |
| Grayscale | four-gray `Ssd1683Driver` | 3-gray on-glass LUTs (`71040ea`) | page 4-level; sheet not stated |
| Panel axes | 800×480 native; touch comment already had 480×800 portrait | 800×480 | page 480×800; sheet no WxH |
| SDMMC width | `busWidth` 4 | 4 | page DAT0–3; sheet TF_DATA0–3 |
| SDMMC comments | | CrossPoint INI + SDK sample.ini still say 1-bit | |
| INI controller comment | SSD1683 | CrossPoint INI still SSD1683; SDK sample.ini SSD1677 | page SSD1677 |
| IMU | `ImuType::None` | still none | BMI270 0x68 |
| LoRa | none | none | Stamp LoRa-1262 / SX1262 |
| NFC | none | none | ST25R3916 0x50 |
| M5IOE1 I2C | (later `M5Ioe1.h`) | probe 0x6F then 0x4F | page 0x4F |
| USB detect | `usbDetect` unassigned | same | sheet GPIO19/20 D−/D+; page has no VBUS GPIO |
| Frontlight / mic / LED | absent on first cut | PMIC-PWM, PDM, discrete RGB | AW9967 / frontlight, PDM, RGB |

Do not treat BMI270 / SX1262 / ST25R3916 as CrossPoint features on this
checkout.
