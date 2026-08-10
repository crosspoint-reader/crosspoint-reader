//! Screens: the contract, the runtime that drives one, and the root views
//! that own a whole screen's chrome.

mod navigation;
mod overlay;

pub use navigation::NavigationScreen;
pub use overlay::{OverlayPanel, Scrim};

use crate::geometry::Point;
use crate::host::{finish_screen, millis, request_update, Button, Input, Renderer};
use crate::view::{InputMask, Interactions, View};

mod driver;
mod routing;

pub use driver::Driver;
use routing::{focused_message, focused_step, resolve};

/// Fire once on press, then repeat after this hold, at this interval.
/// Mirrors `ButtonNavigator` (continuousStartMs / continuousIntervalMs).
const REPEAT_DELAY_MS: u32 = 500;
const REPEAT_INTERVAL_MS: u32 = 500;

/// Buttons the runtime offers a screen before claiming them itself.
const KEYS: [Button; 8] = [
    Button::Left,
    Button::Right,
    Button::Up,
    Button::Down,
    Button::Confirm,
    Button::Back,
    Button::PageBack,
    Button::PageForward,
];

/// A screen.
///
/// ```rust,ignore
/// #[derive(Clone, Copy)]
/// enum Msg { Toggle, Brightness(i32) }
///
/// impl Screen for Panel {
///     type Message = Msg;
///
///     fn body(&self) -> impl View<Msg> {
///         vstack![
///             Toggle::new("Frontlight", self.on).on_change(|_| Msg::Toggle),
///             Slider::new(self.brightness, 100).on_change(Msg::Brightness),
///         ]
///     }
///
///     fn update(&mut self, message: Msg) {
///         match message {
///             Msg::Toggle => self.on = !self.on,
///             Msg::Brightness(v) => self.brightness = v,
///         }
///     }
/// }
/// ```
pub trait Screen {
    /// What this screen's controls send back. One enum per screen, matched
    /// exhaustively in [`update`](Screen::update).
    type Message: Clone;

    /// Describes the screen. A pure function of `self` — no device writes.
    ///
    /// Called once per paint and once per frame that carries input. Built
    /// fresh rather than stored because `loop_` and `render` run on different
    /// FreeRTOS tasks with no lock between them: a stored tree is one task
    /// walking what the other is replacing.
    fn body(&self) -> impl View<Self::Message>;

    /// Applies a message. The only place state changes; the runtime repaints
    /// afterwards, so no screen calls `request_update` itself.
    fn update(&mut self, message: Self::Message);

    /// A key the runtime has not claimed, offered before it applies its own
    /// meaning. Return a message to consume it.
    ///
    /// Consulted **first**, so a screen that wants Up/Down for something other
    /// than moving focus simply says so. Auto-repeat applies to whatever is
    /// claimed here.
    fn on_key(&self, key: Button) -> Option<Self::Message> {
        let _ = key;
        None
    }

    /// A touch that no control claimed. Return a message to consume it.
    ///
    /// An overlay uses this to close when the scrim is tapped.
    fn on_background_tap(&self, point: Point) -> Option<Self::Message> {
        let _ = point;
        None
    }

    /// Whether this screen paints over what is already on the panel, leaving
    /// it visible beneath.
    fn is_overlay(&self) -> bool {
        false
    }

    fn on_enter(&mut self) {}

    fn on_exit(&mut self) {}

    /// The system home gesture. Return `true` to consume it; an overlay does,
    /// so the gesture dismisses the overlay rather than the screen below.
    fn handle_home_gesture(&mut self) -> bool {
        false
    }
}

/// Per-screen state the runtime owns so screens never see it.
struct Repeat {
    button: Option<Button>,
    /// When the current press started, and when it last fired.
    pressed_at: u32,
    fired_at: u32,
}

/// Drives a [`Screen`]: owns focus, input routing, repeat timing and the
/// first-paint guard.
pub struct Runtime<S: Screen> {
    screen: S,
    /// Index into the focusable interactions, in tree order.
    focus: usize,
    /// No touch is routed before the first paint — the tree a touch would be
    /// tested against has not been shown yet. The C++ panel guards the same
    /// way with its `uiReady` flag.
    painted: bool,
    /// Swallows the release that ends a drag, so it cannot also read as a tap.
    dragging: bool,
    repeat: Repeat,
}

impl<S: Screen> Runtime<S> {
    pub fn new(screen: S) -> Self {
        Runtime {
            screen,
            focus: 0,
            painted: false,
            dragging: false,
            repeat: Repeat {
                button: None,
                pressed_at: 0,
                fired_at: 0,
            },
        }
    }

    /// Which interaction holds focus. Exposed only for tests that drive the
    /// runtime directly; screens never see focus at all.
    #[cfg(any(test, feature = "testing"))]
    pub fn focused_index(&self) -> usize {
        self.focus
    }

    /// Builds, measures and collects the tree's interactions.
    fn collect(&self) -> Interactions<S::Message> {
        let mut view = self.screen.body();
        view.measure(Renderer::screen_size());

        let mut out = Interactions::new(self.focus);
        view.interactions(Point::ORIGIN, &mut out);
        out
    }

    /// Applies a message and schedules the repaint the screen would otherwise
    /// have to remember.
    fn dispatch(&mut self, message: S::Message) {
        self.screen.update(message);
        request_update();
    }

    /// Which button, if any, should act this frame — including auto-repeat for
    /// one held down.
    fn active_key(&mut self) -> Option<Button> {
        let now = millis();

        for key in KEYS {
            if Input::was_pressed(key) {
                self.repeat = Repeat {
                    button: Some(key),
                    pressed_at: now,
                    fired_at: now,
                };
                return Some(key);
            }
        }

        // Repeat only while the same button is still down.
        let held = self.repeat.button?;
        if !Input::is_pressed(held) {
            self.repeat.button = None;
            return None;
        }
        if now.wrapping_sub(self.repeat.pressed_at) < REPEAT_DELAY_MS {
            return None;
        }
        if now.wrapping_sub(self.repeat.fired_at) < REPEAT_INTERVAL_MS {
            return None;
        }
        self.repeat.fired_at = now;
        Some(held)
    }

    /// Moves focus, wrapping at both ends. Returns whether anything moved.
    fn move_focus(&mut self, delta: isize, count: usize) -> bool {
        if count == 0 {
            return false;
        }
        let next = (self.focus as isize + delta).rem_euclid(count as isize) as usize;
        if next == self.focus {
            return false;
        }
        self.focus = next;
        true
    }

    /// One frame of input, in priority order. See the module docs.
    fn loop_(&mut self) {
        // -- touch ----------------------------------------------------------
        if self.painted && Input::has_touch() {
            if let Some(point) = Input::touch_held() {
                let interactions = self.collect();
                if let Some(message) = resolve(&interactions, point, InputMask::DRAG) {
                    self.dragging = true;
                    self.dispatch(message);
                    return;
                }
            }

            if Input::touch_released() && self.dragging {
                // Swallow the release that ended a drag: otherwise it reads as
                // a tap elsewhere and, on an overlay, closes the panel.
                self.dragging = false;
                return;
            }

            if let Some(point) = Input::tap() {
                let interactions = self.collect();
                if let Some(message) = resolve(&interactions, point, InputMask::TAP) {
                    self.dispatch(message);
                    return;
                }
                if let Some(message) = self.screen.on_background_tap(point) {
                    self.dispatch(message);
                    return;
                }
            }
        }

        // -- buttons --------------------------------------------------------
        let Some(key) = self.active_key() else { return };

        // The screen gets first refusal, so a screen wanting Up/Down for
        // something other than focus simply claims them.
        if let Some(message) = self.screen.on_key(key) {
            self.dispatch(message);
            return;
        }

        match key {
            Button::Confirm => {
                let interactions = self.collect();
                if let Some(message) = focused_message(&interactions, self.focus) {
                    self.dispatch(message);
                }
            }
            Button::Up | Button::Down => {
                let count = self.collect().focusable_count();
                let delta = if key == Button::Up { -1 } else { 1 };
                if self.move_focus(delta, count) {
                    request_update();
                }
            }
            Button::Left | Button::Right => {
                // Nudge whatever holds focus, so one pair of keys drives every
                // adjustable control instead of the screen wiring them to one.
                let interactions = self.collect();
                let delta = if key == Button::Left { -1 } else { 1 };
                if let Some(message) = focused_step(&interactions, self.focus, delta) {
                    self.dispatch(message);
                }
            }
            Button::Back => finish_screen(),
            _ => {}
        }
    }

    fn render(&mut self) {
        if !self.screen.is_overlay() {
            Renderer::clear();
        }

        let mut view = self.screen.body();
        view.measure(Renderer::screen_size());
        // Interactions run before painting so each widget learns whether it
        // holds focus and can draw itself selected.
        let mut out = Interactions::new(self.focus);
        view.interactions(Point::ORIGIN, &mut out);
        view.render(Point::ORIGIN);

        self.painted = true;
    }
}
