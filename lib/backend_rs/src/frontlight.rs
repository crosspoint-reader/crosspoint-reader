//! The reading light and screen inversion.

use super::raw;

/// The device's frontlight.
///
/// Every method is safe to call on a board without one: the host's driver is
/// inert rather than absent, so a screen does not have to guard each call.
/// Check [`present`](Frontlight::present) to decide whether to show controls at
/// all.
pub struct Frontlight;

impl Frontlight {
    /// Whether this board has a frontlight.
    pub fn present() -> bool {
        unsafe { raw::cpp_frontlight_present() != 0 }
    }

    /// Whether the light has a warmth channel as well as brightness.
    /// Single-channel boards should not show a warmth control.
    pub fn has_warmth() -> bool {
        unsafe { raw::cpp_frontlight_has_color_temperature() != 0 }
    }

    /// Brightness, 0-100.
    pub fn brightness() -> i32 {
        unsafe { raw::cpp_frontlight_brightness() }
    }

    /// Warmth, 0 (cool) to 100 (warm).
    pub fn warmth() -> i32 {
        unsafe { raw::cpp_frontlight_warmth() }
    }

    pub fn is_on() -> bool {
        unsafe { raw::cpp_frontlight_is_on() != 0 }
    }

    /// Sets brightness, clamped to 0-100. Takes effect immediately.
    pub fn set_brightness(percent: i32) {
        unsafe { raw::cpp_frontlight_set_brightness(percent.clamp(0, 100)) }
    }

    /// Sets warmth, clamped to 0-100.
    pub fn set_warmth(percent: i32) {
        unsafe { raw::cpp_frontlight_set_warmth(percent.clamp(0, 100)) }
    }

    pub fn set_on(on: bool) {
        unsafe { raw::cpp_frontlight_set_on(u8::from(on)) }
    }

    /// Persists the current light state.
    ///
    /// Call once when a screen closes, never per adjustment: this writes to
    /// flash, which has a finite erase budget. The host skips the write when
    /// nothing changed, but the call still belongs on an exit path.
    pub fn save() {
        unsafe { raw::cpp_frontlight_save() }
    }
}

/// Screen inversion (dark mode).
pub struct Display;

impl Display {
    pub fn is_inverted() -> bool {
        unsafe { raw::cpp_display_is_inverted() != 0 }
    }

    /// Flips inversion and persists it, returning the new state.
    pub fn toggle_inverted() -> bool {
        unsafe { raw::cpp_display_toggle_inverted() != 0 }
    }
}
