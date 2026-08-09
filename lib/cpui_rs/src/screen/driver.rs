//! A screen with its type erased.
//!
//! [`Screen`](super::Screen) returns `impl View`, so it is deliberately never a
//! trait object. `Runtime<S>` erases the screen type behind this instead, which
//! is what a host dispatches through — see `backend_rs::lifecycle`.

use super::{Runtime, Screen};

/// A screen with its type erased, so a host can drive one without knowing
/// which `Screen` it is. The lifecycle entry points live in the host crate.
pub trait Driver {
    fn on_enter(&mut self);
    fn loop_(&mut self);
    fn on_exit(&mut self);
    fn render(&mut self);
    fn handle_home_gesture(&mut self) -> bool;
}

impl<S: Screen> Driver for Runtime<S> {
    fn on_enter(&mut self) {
        self.screen.on_enter();
    }

    fn loop_(&mut self) {
        Runtime::loop_(self);
    }

    fn on_exit(&mut self) {
        self.screen.on_exit();
    }

    fn render(&mut self) {
        Runtime::render(self);
    }

    fn handle_home_gesture(&mut self) -> bool {
        self.screen.handle_home_gesture()
    }
}
