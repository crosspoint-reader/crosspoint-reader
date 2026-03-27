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
    gpio::{Input, InputConfig, Pull},
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
#[main]
fn main() -> ! {
    let peripherals = esp_hal::init(esp_hal::Config::default());
    let mut adc_config = AdcConfig::new();

    let button_adc_pins = pin_setup_spec();
    let mut adc1_pin_1 = adc_config.enable_pin(peripherals.GPIO1, Attenuation::_11dB);
    let mut adc1_pin_2 = adc_config.enable_pin(peripherals.GPIO2, Attenuation::_11dB);

    // Preserve required firmware semantics from InputManager::begin().
    // - GPIO1: INPUT (analog path -> no pull resistors)
    // - GPIO2: INPUT (analog path -> no pull resistors)
    // - GPIO3: INPUT_PULLUP
    // - ADC attenuation: 11dB
    let power_button = Input::new(peripherals.GPIO3, InputConfig::default().with_pull(Pull::Up));
    println!(
        "setup: GPIO1/2 input-floating for ADC, GPIO3 input-pullup, ADC attenuation=11dB (InputManager::begin())"
    );

    // Validate the required logical setup at startup.
    debug_assert_eq!(button_adc_pins.adc_pins(), &[1, 2]);
    debug_assert_eq!(button_adc_pins.adc_attenuation_db(), 11);
    debug_assert_eq!(button_adc_pins.power_button_pin(), 3);

    debug_assert_eq!(button_adc_pins.adc_pins(), &[BUTTON_ADC_PIN_1, BUTTON_ADC_PIN_2]);
    debug_assert_eq!(button_adc_pins.power_button_pin(), POWER_BUTTON_PIN);

    let mut adc1 = Adc::new(peripherals.ADC1, adc_config);
    let loop_delay = Duration::from_millis(1000);

    fn button_index_from_adc(value: u16, ranges: &[i32], num_buttons: usize) -> Option<u8> {
        let raw = i32::from(value);
        for i in 0..num_buttons {
            if ranges[i + 1] < raw && raw <= ranges[i] {
                return Some(i as u8);
            }
        }
        None
    }

    loop {
        let now = Instant::now();
        let mut pin_1: u16 = 0;
        let mut pin_2: u16 = 0;
        let mut pin_1_samples: u32 = 0;
        let mut pin_2_samples: u32 = 0;

        // Keep the same conversion path as C++ firmware, but average a few samples to reduce noise.
        for _ in 0..4 {
            match block!(adc1.read_oneshot(&mut adc1_pin_1)) {
                Ok(value) => pin_1_samples += u32::from(value),
                Err(_) => {
                    println!("adc1 GPIO1 read failed (oneshot error)");
                    continue;
                }
            }

            match block!(adc1.read_oneshot(&mut adc1_pin_2)) {
                Ok(value) => pin_2_samples += u32::from(value),
                Err(_) => {
                    println!("adc1 GPIO2 read failed (oneshot error)");
                    continue;
                }
            }
        }

        if pin_1_samples > 0 {
            pin_1 = u16::try_from(pin_1_samples / 4).unwrap_or(u16::MAX);
        }

        if pin_2_samples > 0 {
            pin_2 = u16::try_from(pin_2_samples / 4).unwrap_or(u16::MAX);
        }
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
            "adc1={} ({}) adc2={} ({}) power={} (GPIO{})",
            pin_1, btn1, pin_2, btn2, power_state, button_adc_pins.power_button_pin()
        );

        while Instant::now() - now < loop_delay {}
    }
}
