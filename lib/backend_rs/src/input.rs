//! Input: `xpui`'s [`InputSource`] over `MappedInputManager`.
//!
//! Buttons are named by meaning, not position. The firmware remaps them to
//! hardware for the user's front-button layout and the live orientation, so a
//! screen asking for `Back` keeps working however the device is held.
//!
//! The `Button` and `SwipeDir` enums are `xpui`'s — their discriminants are the
//! wire format for these calls, so a second copy here could drift and silently
//! fire the wrong control.

use xpui::geometry::Point;
use xpui::host::InputSource;
use xpui::{Button, SwipeDir};

use crate::firmware::Firmware;
use crate::raw;

impl InputSource for Firmware {
    fn was_pressed(&self, button: Button) -> bool {
        raw::cpp_input_was_pressed(button as u8) != 0
    }

    fn is_pressed(&self, button: Button) -> bool {
        raw::cpp_input_is_pressed(button as u8) != 0
    }

    fn was_released(&self, button: Button) -> bool {
        raw::cpp_input_was_released(button as u8) != 0
    }

    fn has_touch(&self) -> bool {
        raw::cpp_input_has_touch() != 0
    }

    fn tap(&self) -> Option<Point> {
        let mut x = 0;
        let mut y = 0;
        (unsafe { raw::cpp_input_was_tapped(&mut x, &mut y) } != 0).then(|| Point::new(x, y))
    }

    fn touch_held(&self) -> Option<Point> {
        let mut x = 0;
        let mut y = 0;
        (unsafe { raw::cpp_input_touch_held(&mut x, &mut y) } != 0).then(|| Point::new(x, y))
    }

    fn touch_released(&self) -> bool {
        raw::cpp_input_touch_released() != 0
    }

    fn swipe(&self) -> SwipeDir {
        match raw::cpp_input_swipe() {
            1 => SwipeDir::Left,
            2 => SwipeDir::Right,
            3 => SwipeDir::Up,
            4 => SwipeDir::Down,
            _ => SwipeDir::None,
        }
    }

    fn was_back_gesture(&self) -> bool {
        raw::cpp_input_was_back_gesture() != 0
    }

    fn was_home_gesture(&self) -> bool {
        raw::cpp_input_was_home_gesture() != 0
    }
}
