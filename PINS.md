# ESP32-C3 Pin Audit (Xteink X4 firmware)

This document lists the pins explicitly used by the firmware source (`main`, HAL, and open-x4 SDK libs), and how each pin is configured and used at runtime.

## Explicit GPIO/Pin inventory

| Pin | Signal / Function | Module | Config / Mode | Pull | Active sense | Connected to / purpose |
|---|---|---|---|---|---|---|
| GPIO0 | `BAT_GPIO0` (battery voltage ADC) | `BatteryMonitor` + `HalPowerManager::getBatteryPercentage()` | `INPUT` | none (no explicit pull) | N/A (analog read only) | Analog divider from battery sense network; read via `analogReadMilliVolts` or calibrated `analogRead` |
| GPIO1 | `BUTTON_ADC_PIN_1` | `InputManager` | `INPUT` | none (no explicit pull) | Analog thresholds map to front buttons | Front button ladder network (Back/Confirm/Left/Right ADC thresholds). In Rust MVP experiments, reliable `Back` detection required `PULLDOWN` on GPIO1 for this board sample. |
| GPIO2 | `BUTTON_ADC_PIN_2` | `InputManager` | `INPUT` | none (no explicit pull) | Analog thresholds map to front buttons | Front button ladder network (Up/Down ADC thresholds) |
| GPIO3 | `POWER_BUTTON_PIN` | `InputManager` | `INPUT_PULLUP` | `INPUT_PULLUP` | Active LOW (`digitalRead(...) == LOW`) | Physical power button input |
| GPIO4 | `EPD_DC` | `EInkDisplay` | `OUTPUT` | n/a | HIGH = data, LOW = command | Display Data/Command line |
| GPIO5 | `EPD_RST` | `EInkDisplay` | `OUTPUT` | n/a | Active LOW pulse during reset routine | Display reset |
| GPIO6 | `EPD_BUSY` | `EInkDisplay` | `INPUT` | n/a | High during active operation | Display BUSY status input |
| GPIO7 | `SPI_MISO` | Shared SPI bus (`SD` + `eInk`) | set by SPI peripheral | n/a | SPI MISO role | Shared Master-In-Slave-Out line for display and SD |
| GPIO8 | `EPD_SCLK` | Shared SPI bus (`SD` + `eInk`) | set by SPI peripheral | n/a | Clock | SPI SCLK |
| GPIO10 | `EPD_MOSI` | Shared SPI bus (`SD` + `eInk`) | set by SPI peripheral | n/a | Data out from MCU | SPI MOSI |
| GPIO12 | `SD_CS` | `SDCardManager` | set by SdFat (`sd.begin`) | n/a | CS asserted LOW for SD commands | SD card chip-select |
| GPIO13 | `GPIO_SPIWP` (board-specific MOSFET latch control) | `HalPowerManager` (deep sleep) | `GPIO_MODE_OUTPUT` via ESP-IDF | n/a | forced LOW during sleep prep | Drives battery latch MOSFET hold line low before deep sleep (`gpio_hold_en`) |
| GPIO20 | `UART0_RXD` | `HalGPIO::isUsbConnected()` | `INPUT` | none (no explicit pull) | HIGH means USB connected (in this codebase) | USB-connect detect from U0RXD |
| GPIO21 | `EPD_CS` | `EInkDisplay`, SPI init | set by SPI peripheral for CS transactions / high when idle | n/a | Active LOW | Display chip-select |

## Runtime initialization + ownership

- `HalGPIO::begin()`:
  - Initializes button/input subsystem (`inputMgr.begin()`).
  - Initializes SPI bus for screen+SD as `SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS)` i.e. GPIO8/7/10/21.
  - Configures GPIO20 as `INPUT` for USB-detect sampling.

- `InputManager::begin()`:
  - Configures GPIO1, GPIO2 as `INPUT`.
  - Configures GPIO3 as `INPUT_PULLUP`.
  - Sets ADC attenuation to `ADC_11db` (affects button ADC reads).

- `EInkDisplay::begin()`:
  - Reconfigures SPI with `SPI.begin(_sclk, -1, _mosi, _cs)` on GPIO8/10/21.
  - Configures CS/DC/RST as `OUTPUT`, BUSY as `INPUT`.
  - Writes initial pin states: CS=HIGH, DC=HIGH.
  - Uses `digitalRead(_busy)` busy polling with timeout while commands run.
- `HalDisplay`/`EInkDisplay` init nuance:
  - `SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS)` in `HalGPIO::begin()` is the point where the bus explicitly includes MISO (GPIO7).
  - `EInkDisplay::begin()` later uses `SPI.begin(_sclk, -1, _mosi, _cs)`, which keeps pin7 as previously configured.

- `HalPowerManager::begin()`:
  - Configures GPIO0 as `INPUT` for battery ADC.

- `HalPowerManager::startDeepSleep()`:
  - Reconfigures power button GPIO3 to `INPUT_PULLUP` again.
  - Sets GPIO13 to output low and enables hold with `gpio_hold_en(GPIO_NUM_13)` before sleep start.
  - Arms deep-sleep wake source on GPIO3 low with `esp_deep_sleep_enable_gpio_wakeup(1ULL << 3, ESP_GPIO_WAKEUP_GPIO_LOW)`.

## Button decoding details (logical mapping)

- `InputManager` maps ADC values to logical button indices:
  - GPIO1 thresholds define: Back, Confirm, Left, Right (in that order).
  - GPIO2 thresholds define: Up and Down (in that order).
  - GPIO3 maps to `BTN_POWER`, active LOW.
- Logical names consumed by app:
  - `HalGPIO::BTN_BACK = 0`, `BTN_CONFIRM = 1`, `BTN_LEFT = 2`, `BTN_RIGHT = 3`, `BTN_UP = 4`, `BTN_DOWN = 5`, `BTN_POWER = 6`.

## Important cross-connection notes

- SPI is shared between display and SD card by hardware bus lines:
  - Clock/MOSI/MISO: GPIO8/10/7 shared.
  - Chip-selects are separate (`GPIO21` for eInk, `GPIO12` for SD).
- GPIO6 is used by display BUSY **and** is also listed as `EPD_BUSY` in the display docs.
- No explicit UART pin remapping is done in code for `UART0_RXD`/`TX`; only RX is sampled for USB presence.
- No additional GPIOs are configured in firmware beyond those listed in this table.

## Experimental notes (GPIO1 analog/idle-bias)

- In the Rust MVP pull-mode sweep (ESP32-C3), missing-button behavior reproduced only on `GPIO1` channel reads.
- Holding the previously unreliable Back-side buttons while running the 5-second per-combo test showed:
  - `GPIO1` in `float` or `pull_up` -> `adc1_detected=0`, `pin1_raw[min=max=4095]`.
  - `GPIO1` in `pull_down` -> `adc1_detected` rises (e.g. 7–13/20 samples in a 5s window with no manual tap/noise filter), with `pin1_raw[min≈3751..3755, max=4095)` and decoded as `Back`.
- This strongly indicates an idle-bias dependence on GPIO1 for this channel; `GPIO1` can be too floating/high without explicit down-bias for deterministic button decoding.
