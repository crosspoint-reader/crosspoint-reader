# Resource schema

Every file in `resources/` uses this YAML frontmatter so devices can be
compared without reading `BoardConfig.h`. This directory is not a closed board
list. Copy a present file when adding one. The `x4` block below is an example
of the **shape**, not the set.

A file may exist here whose flag is not on this checkout's compile set. Keep
it current anyway. Open it when the task or a compile-set flag names that
device; do not treat unread files as must-build constraints.

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
# Record the *effective* insets. Profiles that omit the field still get the
# struct defaults (top 9, right 3, bottom 3, left 3) — that is not zeros.
viewable_insets: {top: 9, right: 3, bottom: 3, left: 3}

touch: none                    # none | GT911 | FT5x06 | CHSC6x
frontlight: none               # none | pwm | pwm-warm | pmic-pwm
ui_scale: 1.0                  # BoardProfile.uiScale — not DPI
ppi_note: null                 # comment-only in the SDK; do not invent dpi

caps: []                       # derived in BoardConfig.h unless INI overrides
```

There is no `dpi` field on `BoardProfile`. PPI appears only in SDK comments.
`caps` are copied from header defaults; do not require them in CrossPoint INI.

`shared_binary_envs` is the committed envs **on this tree** that set the flag
(`[]` if none). A schema change must update this file and every resource.
