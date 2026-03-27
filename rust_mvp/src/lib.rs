#![no_std]

//! Minimal shared pin-configuration spec for the ESP32-C3 button ADC MVP.

/// Pin configuration constants and types for the ADC/power-button setup.
pub mod pin_config {
    /// Minimal pin-mode abstraction used by the unit tests and to document the required
    /// runtime setup before touching ADC reads.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    #[non_exhaustive]
    pub enum AtLeast {
        /// Floating/input mode with no pull resistor enforced.
        Input,
        /// Input with an explicit internal pull-up enabled.
        InputPullUp,
    }

    /// Exact setup expected from the C3 firmware pin contract.
    #[derive(Debug, Clone, Copy)]
    pub struct ButtonPinConfig {
        adc_pins: [u8; 2],
        adc_mode: AtLeast,
        adc_attenuation_db: u8,
        power_button_pin: u8,
        power_mode: AtLeast,
        power_button_active_low: bool,
    }

    impl ButtonPinConfig {
        pub const fn new(
            adc_pins: [u8; 2],
            adc_mode: AtLeast,
            adc_attenuation_db: u8,
            power_button_pin: u8,
            power_mode: AtLeast,
            power_button_active_low: bool,
        ) -> Self {
            Self {
                adc_pins,
                adc_mode,
                adc_attenuation_db,
                power_button_pin,
                power_mode,
                power_button_active_low,
            }
        }

        pub const fn adc_pins(&self) -> &[u8; 2] {
            &self.adc_pins
        }

        pub const fn adc_mode(&self) -> AtLeast {
            self.adc_mode
        }

        pub const fn adc_attenuation_db(&self) -> u8 {
            self.adc_attenuation_db
        }

        pub const fn power_button_pin(&self) -> u8 {
            self.power_button_pin
        }

        pub const fn power_mode(&self) -> AtLeast {
            self.power_mode
        }

        pub const fn power_button_active_low(&self) -> bool {
            self.power_button_active_low
        }
    }

    /// Returns the exact pin wiring contract required for the MVP.
    pub const fn pin_setup_spec() -> ButtonPinConfig {
        ButtonPinConfig::new([1, 2], AtLeast::Input, 11, 3, AtLeast::InputPullUp, true)
    }

    /// C3 button pin constants mirrored from open-x4 InputManager.
    pub const BUTTON_ADC_PIN_1: u8 = 1;
    pub const BUTTON_ADC_PIN_2: u8 = 2;
    pub const POWER_BUTTON_PIN: u8 = 3;

    /// Same debounce/threshold tables used by the firmware's InputManager.
    pub const ADC_RANGES_1: [i32; 5] = [3800, 3100, 2090, 750, i32::MIN];
    pub const ADC_RANGES_2: [i32; 3] = [3800, 1120, i32::MIN];
}

pub use pin_config::{AtLeast, ButtonPinConfig, pin_setup_spec};
