#![cfg_attr(target_arch = "riscv32", no_std)]
#![cfg_attr(target_arch = "riscv32", no_main)]

#[cfg(not(target_arch = "riscv32"))]
fn main() {
    println!(
        "ESP32-C3 firmware target only: build with a riscv32 target and espflash to run on device."
    );
}

#[cfg(target_arch = "riscv32")]
use esp_hal::{
    analog::adc::{Adc, AdcConfig, Attenuation},
    delay::Delay,
    gpio::{Input, InputConfig, Pull, RtcPinWithResistors},
    time::{Duration, Instant},
    main,
};

#[cfg(target_arch = "riscv32")]
esp_bootloader_esp_idf::esp_app_desc!();

#[cfg(target_arch = "riscv32")]
use esp_println::println;

#[cfg(target_arch = "riscv32")]
use nb::block;

#[cfg(target_arch = "riscv32")]
use esp32c3_button_adc_mvp::pin_config::{
    pin_setup_spec, BUTTON_ADC_PIN_1, BUTTON_ADC_PIN_2, POWER_BUTTON_PIN, ADC_RANGES_1, ADC_RANGES_2,
};

#[cfg(target_arch = "riscv32")]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    loop {}
}

#[cfg(target_arch = "riscv32")]
#[derive(Debug, Clone, Copy)]
enum ResistorState {
    Floating,
    PullUp,
    PullDown,
}

#[cfg(target_arch = "riscv32")]
impl ResistorState {
    fn as_label(&self) -> &'static str {
        match self {
            Self::Floating => "float",
            Self::PullUp => "pull_up",
            Self::PullDown => "pull_down",
        }
    }

    fn as_input_config(self) -> InputConfig {
        match self {
            Self::Floating => InputConfig::default(),
            Self::PullUp => InputConfig::default().with_pull(Pull::Up),
            Self::PullDown => InputConfig::default().with_pull(Pull::Down),
        }
    }
}

#[cfg(target_arch = "riscv32")]
#[derive(Debug, Clone, Copy)]
struct PinCombination {
    name: &'static str,
    adc_pin1: ResistorState,
    adc_pin2: ResistorState,
    power_pin3: ResistorState,
    usb_pin20: ResistorState,
}

#[cfg(target_arch = "riscv32")]
impl PinCombination {
    fn print_label(&self) {
        println!(
            "configuration: GPIO1: {} | GPIO2: {} | GPIO3: {} | GPIO20: {}",
            self.adc_pin1.as_label(),
            self.adc_pin2.as_label(),
            self.power_pin3.as_label(),
            self.usb_pin20.as_label(),
        )
    }
}

#[cfg(target_arch = "riscv32")]
#[inline]
fn apply_rtc_pin_pull<Pin>(pin: &mut Pin, mode: ResistorState)
where
    Pin: RtcPinWithResistors,
{
    match mode {
        ResistorState::Floating => {
            pin.rtcio_pullup(false);
            pin.rtcio_pulldown(false);
        }
        ResistorState::PullUp => {
            pin.rtcio_pullup(true);
            pin.rtcio_pulldown(false);
        }
        ResistorState::PullDown => {
            pin.rtcio_pullup(false);
            pin.rtcio_pulldown(true);
        }
    }
}

#[cfg(target_arch = "riscv32")]
#[main]
fn main() -> ! {
    let peripherals = esp_hal::init(esp_hal::Config::default());
    let mut adc_config = AdcConfig::new();
    let delay = Delay::new();

    let button_adc_pins = pin_setup_spec();
    // Match full `HalGPIO::begin()` pin setup: USB detect input is configured before
    // ADC/button setup in the production firmware.
    let mut usb_detect = Input::new(peripherals.GPIO20, InputConfig::default());

    // Match C++ `InputManager::begin()` and clear potential RTC pull state on the ADC lines.
    peripherals.GPIO1.rtcio_pullup(false);
    peripherals.GPIO1.rtcio_pulldown(false);
    peripherals.GPIO2.rtcio_pullup(false);
    peripherals.GPIO2.rtcio_pulldown(false);

    let mut adc1_pin_1 = adc_config.enable_pin(peripherals.GPIO1, Attenuation::_11dB);
    let mut adc1_pin_2 = adc_config.enable_pin(peripherals.GPIO2, Attenuation::_11dB);

    peripherals.GPIO3.rtcio_pulldown(false);

    // Preserve required firmware semantics from InputManager::begin().
    // - GPIO1: INPUT (analog path -> no pull resistors)
    // - GPIO2: INPUT (analog path -> no pull resistors)
    // - GPIO3: INPUT_PULLUP
    // - ADC attenuation: 11dB
    let mut power_button = Input::new(peripherals.GPIO3, InputConfig::default().with_pull(Pull::Up));
    // Next experiment: isolate GPIO1 state impact while keeping other pins at baseline behavior.
    let combos = [
        PinCombination {
            name: "g1-float-g2-float-g3-up",
            adc_pin1: ResistorState::Floating,
            adc_pin2: ResistorState::Floating,
            power_pin3: ResistorState::PullUp,
            usb_pin20: ResistorState::Floating,
        },
        PinCombination {
            name: "g1-up-g2-float-g3-up",
            adc_pin1: ResistorState::PullUp,
            adc_pin2: ResistorState::Floating,
            power_pin3: ResistorState::PullUp,
            usb_pin20: ResistorState::Floating,
        },
        PinCombination {
            name: "g1-down-g2-float-g3-up",
            adc_pin1: ResistorState::PullDown,
            adc_pin2: ResistorState::Floating,
            power_pin3: ResistorState::PullUp,
            usb_pin20: ResistorState::Floating,
        },
    ];

    println!(
        "setup: InputManager::begin() parity checks done; entering 5-second pull-mode experiment loop"
    );
    println!("GPIO1 = ADC ADC1_CH1, GPIO2 = ADC ADC1_CH2, GPIO3 = power input, GPIO20 = USB detect");

    // Validate the required logical setup at startup.
    debug_assert_eq!(button_adc_pins.adc_pins(), &[1, 2]);
    debug_assert_eq!(button_adc_pins.adc_attenuation_db(), 11);
    debug_assert_eq!(button_adc_pins.power_button_pin(), 3);

    debug_assert_eq!(button_adc_pins.adc_pins(), &[BUTTON_ADC_PIN_1, BUTTON_ADC_PIN_2]);
    debug_assert_eq!(button_adc_pins.power_button_pin(), POWER_BUTTON_PIN);

    fn button_index_from_adc(value: u16, ranges: &[i32], num_buttons: usize) -> Option<u8> {
        let raw = i32::from(value);
        for i in 0..num_buttons {
            if ranges[i + 1] < raw && raw <= ranges[i] {
                return Some(i as u8);
            }
        }
        None
    }

    let mut adc1 = Adc::new(peripherals.ADC1, adc_config);
    let loop_delay = Duration::from_millis(1_000);
    let experiment_window = Duration::from_millis(5_000);

    loop {
        for combo in combos {
            apply_rtc_pin_pull(&mut adc1_pin_1.pin, combo.adc_pin1);
            apply_rtc_pin_pull(&mut adc1_pin_2.pin, combo.adc_pin2);
            power_button.apply_config(&combo.power_pin3.as_input_config());
            usb_detect.apply_config(&combo.usb_pin20.as_input_config());

            println!();
            println!("=== EXPERIMENT: {} ===", combo.name);
            combo.print_label();
            let window_start = Instant::now();

            while Instant::now() - window_start < experiment_window {
                let now = Instant::now();
                let pin_1 = match block!(adc1.read_oneshot(&mut adc1_pin_1)) {
                    Ok(value) => value,
                    Err(_) => {
                        println!("adc1 GPIO1 read failed (oneshot error)");
                        0
                    }
                };

                // Small settle delay to reduce cross-channel coupling between ladder reads.
                delay.delay_micros(50);

                let pin_2 = match block!(adc1.read_oneshot(&mut adc1_pin_2)) {
                    Ok(value) => value,
                    Err(_) => {
                        println!("adc2 GPIO2 read failed (oneshot error)");
                        0
                    }
                };

                let power_state = if power_button.is_low() { "LOW" } else { "HIGH" };
                let btn1 = button_index_from_adc(pin_1, &ADC_RANGES_1, 4).map_or("none", |idx| match idx {
                    0 => "Back",
                    1 => "Confirm",
                    2 => "Left",
                    _ => "Right",
                });
                let btn2 = button_index_from_adc(pin_2, &ADC_RANGES_2, 2).map_or("none", |idx| match idx {
                    0 => "Up",
                    _ => "Down",
                });

                println!(
                    "adc1={} ({}) adc2={} ({}) power={} (GPIO{}), usb={} | delta={:?}",
                    pin_1,
                    btn1,
                    pin_2,
                    btn2,
                    power_state,
                    button_adc_pins.power_button_pin(),
                    usb_detect.is_low(),
                    Instant::now() - now
                );

                while Instant::now() - now < loop_delay {}
            }
        }
    }
}
