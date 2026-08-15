# Resource schema

Every file in `resources/` uses this YAML frontmatter so devices can be
compared without reading `BoardConfig.h`. Copy a present file when adding one.

```yaml
device: x4
device_flag: FREEINK_DEVICE_X4
sdk_profile: XTEINK_X4
sdk_header: freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h
shared_binary_envs: [default, gh_release, gh_release_rc, slim]

mcu_family: c3                 # c3 | s3 | esp32
board_package: esp32-c3-devkitm-1
psram_in_ini: false            # -DBOARD_HAS_PSRAM on those envs
psram_on_silicon: false
fb_in_psram: false             # FREEINK_FB_PSRAM for this device
sdmmc: false
block_device_interface: false

width: 800
height: 480
fb_bytes: 48000                # this profile; dual-device MAX may be larger
controllers: [SSD1677]
grayscale: 4-level
# Bezel overlap in the panel-native frame (BoardProfile.viewableInsets).
# Keep on-screen UI (status, gutters, margins) out of these pixels.
viewable_insets: {top: 9, right: 3, bottom: 3, left: 3}

touch: none                    # none | GT911 | FT5x06 | CHSC6x
frontlight: none               # none | pwm | pwm-warm | pmic-pwm
ui_scale: 1.0                  # BoardProfile.uiScale — not DPI
ppi_note: null                 # comment-only in the SDK; do not invent dpi

caps: []                       # derived in BoardConfig.h unless INI overrides
present_in_ini: true           # any committed env sets this device flag
```

There is no `dpi` field on `BoardProfile`. PPI appears only in SDK comments.
`caps` are copied from header defaults; do not require them in CrossPoint INI.

`present_in_ini: false` means dormant: keep the file, do not apply it as a
compile target until a committed env and a CI job exist.
