# Board Support

The firmware builds for four boards from one source tree. The SDK (`freeink-sdk`, a git
submodule) describes each board in a `BoardProfile` in `BoardConfig.h`. The firmware reads the
active profile through `BoardConfig::ACTIVE` and does not hardcode pins.

| Board | MCU | Panel | Input | Dev env |
|---|---|---|---|---|
| Xteink X3 | ESP32-C3 | UC8253 (some batches UC8279), 792x528 | ADC button ladder, tilt sensor | `default` |
| Xteink X4 | ESP32-C3 | SSD1677, 800x480 | ADC button ladder | `default` |
| Xteink X4 Pro | ESP32-S3, 8 MB PSRAM | SSD1677, UC8179 or UC8279 depending on batch, 800x480 | GT911 touch, capacitive Home key, two nav keys | `x4pro` |
| LilyGo T5S3 Pro | ESP32-S3, 8 MB PSRAM | 4.7" 960x540 parallel panel, driven through LovyanGFX | GT911 touch, capacitive Home key, one user key | `lilygo_t5s3` |

X3 and X4 share one binary and are told apart at boot (`HalGPIO::begin()`,
`BoardConfig::selectDevice()`). The release env names are in
[Getting Started](./getting-started.md#build). Pin maps and hardware evidence live upstream; see
[Upstream references](#upstream-references). Touch is covered in
[Touch Architecture](./touch-architecture.md).

## Ask what the board can do

Code that behaves differently per board asks a capability question, not which board it is
running on. The predicates live in `lib/hal/HalCapabilities.h` and read the active profile:
`hasHardwareRtc()`, `rtcType()`, `hasTiltSensor()`, `hasI2cFuelGauge()`, `hasAdcBattery()`,
`hasTouch()`, `hasHomeKey()`, `hasBackAndConfirmButtons()`, `inputStyle()`, `hasFrontlight()`,
`hasColorTemperature()`, `sdUsesSpi()`, `panelNeedsHalfRefreshSettle()` and a few more. Settings
follow the same rule: a setting declares the hardware it needs as a `SettingRequires` value
(`SettingInfo.h`), never a board.

Why: `HalGPIO::deviceIsX3()` only tells the two C3 boards apart. On an S3 build `_deviceType` is
never assigned and keeps its default, `X4`, so every `deviceIsX3()` reads false and every
`deviceIsX4()` reads true. A board-name condition does not fail when a board is added. It fails
silently, often later, when the board gains a capability the condition never anticipated: giving
the T5S3 its RTC broke two checks that spelled "has an RTC" as `!deviceIsX3()`. Widening the enum
to name every board multiplies the problem instead of removing it.

Rules:

- Add a predicate to `HalCapabilities.h` when no existing one answers your question. A pure
  addition should leave the `default` build byte-identical; convert call sites in a separate
  commit.
- Board identity is right only for identity: the X3/X4 runtime detection and `selectDevice()`,
  X3-specific USB polling and wake handling, and the GPIO13 battery latch
  (`HalGPIO::isXteinkDevice()`). Genuine silicon quirks key on `ACTIVE.displayController`, with a
  comment saying why; `panelNeedsHalfRefreshSettle()` is the model. Board-support glue that is
  per-board by nature uses the one `BOARD_SUPPORT_OWNS_BUSES` macro.
- Presence is not protocol. `hasHardwareRtc()` is true on the X3 (DS3231) and on the X4 Pro and
  T5S3 (PCF8563 family), which use different registers, so `HalClock` dispatches on `rtcType()`.
  The fuel gauge is the same: BQ27220 on the X3 and T5S3, CW2017 on the X4 Pro, dispatched by the
  SDK's `BatteryMonitor` on the profile's gauge type.
- A profile can carry data a chip forgets. The BQ27220 keeps Design Capacity in RAM, and a gauge
  that lost power comes back at TI's 3000 mAh default and learns a full-charge capacity against it.
  `HalPowerManager::serviceGaugeCapacityLoad()`, called once per loop pass from `main.cpp`, drives
  the SDK's load (`BatteryMonitor::loadDesignCapacity()`) one short step at a time over about 8 s,
  holding `HalI2cBus::Lock` for each step. The SDK writes only when the gauge needs it. Only the X3
  profiles set `designCapacityMah` (650 mAh), so every other board returns on the first call.
- On the ADC-ladder boards (X3, X4) the `InputPins` fields hold ladder indices, not GPIO
  numbers. A test that reads them as pins, for presence or against a GPIO number, answers a
  different question there. Check `inputStyle()` first; `upKeyIsBootStrap()` shows the pattern.

The remaining board-name checks are listed in
[future work](../future_work/boards-and-touch.md#board-name-checks-still-to-convert).

## Build environments

`platformio.ini` composes each env from three layers:

- `[base]`: everything shared, including the SDK hardware libraries.
- `[c3]` or `[s3]`: the MCU family. `[c3]` selects `FREEINK_DEVICE_X4` and `FREEINK_DEVICE_X3`,
  sets `CP_TOUCH_UI=0` and the RISC-V wolfSSL assembly (`WOLFSSL_SP_RISCV32`, which the Xtensa
  assembler rejects); `[s3]` uses `WOLFSSL_SP_SMALL`. `BoardConfig.h` stops the build with
  `#error` if the selected devices span two MCU families, so an env takes exactly one of these.
- `[x4pro_board]` or `[lilygo_board]`: the board, shared by its dev and release envs. The envs
  add only the version string and log level.

The S3 board sections extend `base` and never `firmware_tuned`; the comment on `[firmware_tuned]`
explains why (USB mass storage needs the prebuilt TinyUSB component).

SDK capability macros (`FREEINK_CAP_TOUCH`, `FREEINK_CAP_FRONTLIGHT`, `FREEINK_CAP_WARMLIGHT`,
`FREEINK_CAP_RTC`, ...) are derived in `BoardConfig.h` from the device flags, and an env can
override them with `-DFREEINK_CAP_*`. An SDK hardware library whose macro is off links stub
bodies. That is the RTC trap: when the T5S3 was brought up, `FREEINK_DEVICE_LILYGO` was missing
from the default `FREEINK_CAP_RTC` list, so the `Rtc` library linked stubs and `begin()` always
returned false even with a correct profile. `[lilygo_board]` still sets `-DFREEINK_CAP_RTC=1`
explicitly. Giving a board a peripheral therefore takes three things: the profile entry, the
capability macro, and a HAL path that dispatches on the right type.

## Bus ownership

**I2C.** `HalI2cBus` owns both start-up and serialisation of the shared I2C bus.

- `HalI2cBus::ensureBusStarted()` runs once in `setup()`, right after `gpio.begin()`. On a touch
  board the SDK's `InputManager` has already started the bus for the GT911 during
  `inputMgr.begin()`. A second `Wire.begin()` on the same port breaks the ESP-IDF `i2c_master`
  driver, and every later transaction fails with `ESP_ERR_INVALID_STATE`. So the owner stands down
  when touch already holds those pins or when `BOARD_SUPPORT_OWNS_BUSES` is set (T5S3), and
  otherwise starts the bus from the gauge or sensor pins in the profile (X3). Where touch owns the
  bus, the gauge runs at the touch driver's clock.
- `HalI2cBus::Lock` serialises access across tasks. Why that is needed, and why it compiles to
  nothing on the C3, is in
  [Touch Architecture](./touch-architecture.md#i2c-bus-and-the-input-sampler).
- The T5S3's board-support layer drives its PCA9535 expander and TPS65185 panel PMIC under a
  mutex of its own, from LovyanGFX's panel task on every refresh. `main.cpp` passes that mutex to
  `HalI2cBus::adoptMutex()`, so one lock covers the whole bus. With two mutexes a collided rail
  power-up clocks a frame out with the high-voltage rails down.

Never call `Wire.begin()` from a peripheral driver: a peripheral is one user of a shared bus, not
its owner.

**SPI.** Only the C3 pre-claims the SPI bus, in `HalGPIO::begin()`, because there the panel and
the SD card share it and nothing else claims it. `SPIClass::begin()` keeps the first caller's
pins, and on every other board the SDK display driver, `SDCardManager` or the board-support layer
gets there first. `HalStorage` takes the SPI bus lock only when the SD card really shares the
display's SPI bus (`sdSharesDisplaySpiBus()`); the X4 Pro's SD card is on SDMMC and shares
nothing.

## Dual-core S3

The C3 is single-core RISC-V and the S3 dual-core Xtensa. Code that only held because of the C3:

- `taskENTER_CRITICAL(nullptr)`. The single-core port ignores the mux and disables interrupts;
  the dual-core port takes a real spinlock and asserts it is not null. It boot-looped the X4 Pro.
  Always pass a real `portMUX_TYPE` (`activityStateMux` in `ActivityManager.cpp`).
- Unpinned tasks. `ActivityManager::begin()` pins the render task to core 1 on dual-core parts,
  so long renders and cover decodes stay off CPU 0's idle watchdog. Decide the core for any new
  long-running task.
- Architecture-specific code. Deep-sleep wake branches on SoC capability
  (`SOC_PM_SUPPORT_EXT1_WAKEUP` or `SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP`, in `HalPowerManager`), and
  the panic backtrace in `HalSystem.cpp` on `__riscv` or `__XTENSA__`. Both have an `#error`
  default. Branch on capability, not on the chip name.

## PSRAM

Both S3 board sections set `-DBOARD_HAS_PSRAM` and select the PSRAM variant through
`board_build.arduino.memory_type` (`dio_opi` on the X4 Pro; `qio_opi` with `flash_mode = qio` on
the T5S3). The T5S3 cannot run without PSRAM: its 960x540 framebuffer does not fit in internal
RAM.

No code asks for PSRAM explicitly. The prebuilt Arduino variants set `CONFIG_SPIRAM_USE_MALLOC`
with `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`, so on the S3 boards every plain allocation over
4 KB lands in PSRAM first and smaller ones stay internal. That is the allocator's default, not a
policy. Two consequences:

- Internal RAM is the scarce pool on S3. Heap watermarks there read
  `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)` (`scarceHeapWatermark()` in
  `EpubReaderActivity.cpp`); a total that includes 8 MB of PSRAM hides real dips.
- Never change the CPU clock on a PSRAM board. The PSRAM clock derives from the CPU/APB clock, and
  switching it while anything touches PSRAM corrupts the access (seen on the T5S3 as
  `TG1WDT_SYS_RST` right after "Going to low-power mode"). Every `setCpuFrequencyMhz()` call in
  `HalPowerManager` is gated on `CPU_SCALING_ALLOWED`, which is false under `BOARD_HAS_PSRAM`.

## Display: ask the driver

The reader picks its anti-aliasing strategy from what the display path can do, not from the board:

- single push, when `renderer.supportsGrayFrame()` (today only the T5S3's LovyanGFX path): the
  gray planes are captured during the page render and the page goes out in one graded waveform;
- inline, when `renderer.supportsAsyncRefresh()` and the async refresh frees the frame buffer
  (`!asyncRefreshHoldsFrameBuffer()`): the LSB plane is
  rendered while the B/W waveform runs (X4);
- deferred: everything else, including the X3 and any panel that cannot overlap a refresh.

`EpubReaderActivity::usesDeferredAa()` holds the inline/deferred split. It used to be
`!renderer.isX3()`, which sent the T5S3 down the inline path on a driver that cannot overlap, so it
paid every AA write on top of a blocking refresh. The single-push design is described in
[Touch Architecture](./touch-architecture.md#t5s3-single-push-grayscale) and
[LilyGo T5 S3 display stack](../lilygo-t5s3-display-stack.md).

The X4 Pro's panel controller varies by production batch. `HalDisplay::begin()` calls
`freeink::applyXteinkDisplayController()` on non-C3 boards to resolve it before the driver is
chosen; without it a UC81xx unit is sent SSD1677 command streams and shows no image.

## Xteink X4 Pro

- Keys: two nav keys on GPIO0 and GPIO7, mapped to Up and Down (the page pair), and Power on
  GPIO3, all active-low digital inputs. There is no Back, Confirm, Left or Right key: Back and
  Confirm come from the capacitive Home key (tap = Confirm, hold = Back, routed in `HalGPIO`) and
  from touch. GPIO0 is the ESP32-S3 boot strap, and holding it through a reset enters ROM download
  mode, so no boot-time key combination may use it (`HalCapabilities::upKeyIsBootStrap()`).
- I2C: GT911 touch (0x5D), BM8563 RTC (0x51, PCF8563-compatible) and CW2017 gauge (0x63), all on
  bus 0, SDA39/SCL38.
- SD card: native SDMMC, 1-bit, mounted by the SDK's `SdmmcBlockDevice`
  (`USE_BLOCK_DEVICE_INTERFACE=1`).
- USB: no detect pin is known (`usbDetect` is unassigned), so USB presence comes only from USB
  start-of-frame activity.
- Frontlight: two LEDC PWM channels, `FrontlightConfig{8, 25000, 10, true, 9}` in the SDK profile.
  - GPIO8 is the primary (brightness, cool) channel and GPIO9 the warm channel. The SDK records
    this as confirmed on hardware; if a unit ever shows the warmth direction inverted, the pair is
    reversed in the profile.
  - 25 kHz, 10-bit, both channels active-high (init drives the pin low for off). The OEM bring-up
    dump used 10 kHz; stock firmware 7.0.8 uses 25 kHz, and the SDK follows it.
  - The OEM firmware uses LEDC channels 4 and 5.
  - OEM NVS keys, should anyone import stock settings: `lightWarmValue`, `lightColdValue`,
    `lightCT`, `lightBri`, `lightOn`.
  - `FREEINK_CAP_WARMLIGHT` is derived from `FREEINK_DEVICE_X4PRO`; at runtime
    `HalCapabilities::hasColorTemperature()` reads the profile's `gpioWarm`.

## LilyGo T5S3

- Board-support layer: `BoardT5S3::begin()` (SDK `libs/hardware/BoardT5S3`) runs first in
  `setup()`, before `gpio.begin()`. It starts the I2C and SD SPI buses, deselects the LoRa radio
  that shares the SD bus (an undriven chip select there corrupts SD transactions), powers down
  LoRa and GPS, configures BOOT, and registers the expander's user button through
  `InputManager::setButtonHook()`. Without it the board has no working buttons.
- Keys:

  | Key | Wiring | Role |
  |---|---|---|
  | BOOT | GPIO0 | Power |
  | User key (silkscreen "IO48") | PCA9535 expander IO1_2, read by the button hook | Down |
  | Home | GT911 capacitive key, status bit 0x10 | tap = Confirm, hold = Back |
  | PWR | on no GPIO and no expander line (vendor pin map) | not readable; probably the BQ25896 charger's /QON input, unproven |
  | RST | ESP32 EN pin | hardware reset |

- Why the Home key carries Back: the board has no Back key, and no `BUTTON_ACTION` produces Back,
  so Back has to come from the input-mapping layer. A hold rather than a double tap, because any
  double action makes `ButtonEventManager` delay every single click by its double-click window
  (`DOUBLE_WINDOW_MS`, 300 ms). `HalGPIO::begin()` routes the key only to the roles the board has
  no pin for, and `HalGPIO::sampleOnce()` injects it into the button accumulators before they are
  latched, so `wasPressed()` readers and `ButtonEventManager` both see it. The hold fires on the
  SDK's long-press signal (`HOME_KEY_LONG_PRESS_MS`, 700 ms), which arrives while the key is still
  down and is never followed by a tap.
- Vendor button lists are a lower bound. The vendor wiki does not mention the Home key; it was
  found by tracing the GT911 status bit. Probe the hardware.
- Reset button: the board has no USB-detect pin, so `HalGPIO::getWakeupReason()` once read a RST
  press (a power-on reset) as a power-button wake, the wake check failed, and `setup()` put the
  board straight back to sleep. It looked frozen. The power-wake inference now requires a usable
  USB-detect pin. Flash mode was ruled out: qio and dio behave the same.
- I2C: GT911 (0x5D), BQ27220 gauge (0x55), BQ25896 charger (0x6B), PCF8563 RTC (0x51), plus the
  board layer's PCA9535 and TPS65185, on SDA39/SCL40.
- SD card over SPI (SCLK14, MISO21, MOSI13, CS12), shared with the LoRa radio and held at 20 MHz
  by `HalStorage::begin()` (see
  [future work](../future_work/boards-and-touch.md#sd-clock-on-the-t5s3)).
- Frontlight: one channel, a PT4103 boost enable on GPIO11, 1 kHz and 12-bit. The SDK profile
  explains why not 5 kHz.

## Gotchas

- A `platformio.local.ini` (gitignored, loaded through `extra_configs`) whose env builds on
  `${base.build_flags}` selects no device. `BoardConfig.h` then reports "no device selected" and
  "all selected devices must share one MCU family" together, which looks like a submodule or
  toolchain problem. Build on `${c3.build_flags}` or a board section instead.
- The MCU-family `#error` fires in the first SDK translation unit, so a misconfigured env fails
  with an SDK error rather than a configuration error.
- `pio run ... | tail` reports `tail`'s exit code. Search the output for `SUCCESS` or `FAILED`.
- Judge flash use by the `Flash:` line of a real build, not by text size. Shared additions are
  charged against the C3, which has the fullest image.
- Bump the SDK submodule in its own commit, with its own `default` build. After an SDK pull,
  restart any build in progress; it would mix old and new headers.
- `BoardProfile` is initialised positionally. The fork's own field, `panelFadesInSunlight`, is kept
  last, so a field upstream appends goes before it and the profiles that set it need a placeholder
  for the new field. Without one, the X3/X4 `true` brace-elides into the new field with no
  diagnostic. `static_assert`s after the Xteink profiles in `BoardConfig.h` fail the build on that
  drift; keep them when merging upstream.
- On a new board, make serial logging work first. Without a log every other fault is guesswork.
  The earliest boot lines are printed before the USB serial port comes back after a reset, so a
  capture over the device's own USB misses them; use a UART adapter for those.
- Deep-sleep wake is hardcoded active-low (`ESP_EXT1_WAKEUP_ANY_LOW` on the S3,
  `ESP_GPIO_WAKEUP_GPIO_LOW` on the C3), while light sleep reads `input.powerActiveHigh`. Every
  board we build has an active-low power key; a board with an active-high one needs the
  deep-sleep path converted.
- Keep a known-good upstream build at hand. Flashing `upstream/develop` on the same board as a
  control turns a guess into a comparison; it settled the X4 Pro's dual-core crash.
- On a board far from the one the code was written for, a fix that changes nothing visible may
  have uncovered the next fault. The T5S3's anti-aliasing problems came in three layers, each
  visible only once the one above was fixed.

## Verifying a board change

```sh
pio run -e default       # X3 and X4, the regression gate
pio run -e x4pro
pio run -e lilygo_t5s3
```

CI builds the same three envs (`.github/workflows/ci.yml`).

- A change meant for the S3 boards only, or a new capability predicate, should leave the
  `default` build byte-identical. Compare its `RAM:` and `Flash:` lines with and without the
  change.
- Run the host tests ([Testing and Debugging](./testing-debugging.md#host-tests)). The touch
  geometry suites are listed in
  [Touch Architecture](./touch-architecture.md#how-screens-receive-touch).
- A wrong pin is a black screen, not a compile error. After changing how a C3 board sources pins,
  flash an X3 or X4 and check that it boots to Home, the SD card mounts and a book opens, the
  battery reading is sane (gauge over I2C on the X3, ADC on the X4), and Settings shows the same
  entries as before (tilt and Fast AA only on the X3).
- Flash every board whose behaviour changed. Builds and host tests cannot see what a screen shows
  or what a thumb can reach.

## Upstream references

- SDK board notes, in our SDK fork:
  [X4 Pro](https://github.com/jpirnay/freeink-sdk/blob/witchhunt/docs/xteink-x4pro-support.md)
  and [LilyGo T5S3](https://github.com/jpirnay/freeink-sdk/blob/witchhunt/docs/lilygo-t5s3-support.md).
- Board profiles: `libs/hardware/BoardConfig/include/BoardConfig.h` in the SDK.
- LilyGo T5S3 vendor repository,
  [`Xinyuan-LilyGO/T5S3-4.7-e-paper-PRO`](https://github.com/Xinyuan-LilyGO/T5S3-4.7-e-paper-PRO).
  The pin map is
  [`docs/pinmap.md`](https://github.com/Xinyuan-LilyGO/T5S3-4.7-e-paper-PRO/blob/main/docs/pinmap.md),
  compiled from the schematic under `hardware/`. Where the vendor README and the schematic
  disagree (the README names a PCF85063 RTC; the board carries a PCF8563TS), trust the schematic.
