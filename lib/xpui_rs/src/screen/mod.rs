//! Screens: the contract, the runtime that drives one, and the root views
//! that own a whole screen's chrome.

mod navigation;
mod overlay;

pub use navigation::NavigationScreen;
pub use overlay::OverlayPanel;

use crate::geometry::Point;
use crate::host::{Button, Input, Renderer, SwipeDir, finish_screen, millis, request_update};
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

    /// A swipe, offered before the runtime gives it its own meaning.
    /// Return a message to consume it.
    ///
    /// Consulted **first**, so a screen that pages on a swipe — a reader, say —
    /// simply claims it and the runtime does not move focus.
    fn on_swipe(&self, _direction: SwipeDir) -> Option<Self::Message> {
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
    /// Where focus sat before a view captured input, so dismissing a dialog
    /// returns to the row that opened it rather than wherever its index landed.
    focus_before_capture: Option<usize>,
    /// How far the screen's scrolling view is scrolled. Owned here because the
    /// view tree is rebuilt every frame and could not remember it, and because
    /// keeping focus visible is the runtime's job — a screen never sees it.
    scroll: i32,
    repeat: Repeat,
}

impl<S: Screen> Runtime<S> {
    pub fn new(screen: S) -> Self {
        Runtime {
            screen,
            focus: 0,
            painted: false,
            dragging: false,
            focus_before_capture: None,
            scroll: 0,
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

    /// Moves focus in or out of a capturing view. Returns whether it moved.
    ///
    /// Entering capture parks the outer focus and adopts the view's preference —
    /// a picker opens on the value already chosen. Leaving puts the old focus
    /// back, so dismissing a dialog returns to the row that opened it rather
    /// than wherever its index happened to land.
    fn adopt_capture(&mut self, interactions: &Interactions<S::Message>) -> bool {
        match interactions.captured_focus() {
            Some(preferred) if self.focus_before_capture.is_none() => {
                self.focus_before_capture = Some(self.focus);
                self.focus = preferred.min(interactions.focusable_count().saturating_sub(1));
                true
            }
            None => match self.focus_before_capture.take() {
                Some(previous) => {
                    self.focus = previous;
                    true
                }
                None => false,
            },
            _ => false,
        }
    }

    /// Scrolls the minimum needed to bring the focused interaction fully into
    /// view. Returns whether the offset moved.
    ///
    /// Minimum rather than centring: on a panel that repaints whole and takes a
    /// second or more, the less that shifts under the eye the better.
    fn settle_scroll(&mut self, interactions: &Interactions<S::Message>) -> bool {
        let Some((viewport, content_height)) = interactions.viewport() else {
            return false;
        };
        let Some(focused) = interactions.focused_rect(self.focus) else {
            return false;
        };

        let last = interactions.focusable_count().saturating_sub(1);
        let mut next = self.scroll;

        if focused.bottom() > viewport.bottom() {
            next += focused.bottom() - viewport.bottom();
        }
        if focused.origin.y < viewport.origin.y {
            next -= viewport.origin.y - focused.origin.y;
        }

        // Scrolling the bare minimum leaves whatever sits above the first
        // control — a section title, typically — stranded off the top edge,
        // with nothing focusable up there to scroll back to. The ends are
        // special: reaching the first control means the top of the content,
        // and the last means the bottom.
        if self.focus == 0 {
            next = 0;
        } else if self.focus == last {
            next = content_height - viewport.size.height;
        }

        let next = next.clamp(0, (content_height - viewport.size.height).max(0));
        if next == self.scroll {
            return false;
        }
        self.scroll = next;
        true
    }

    /// Collects, then settles focus against whatever captured input this frame,
    /// so every caller — Confirm, Up/Down, taps — reads the same table.
    fn collect_settled(&mut self) -> Interactions<S::Message> {
        let interactions = self.collect();
        if self.adopt_capture(&interactions) {
            return self.collect();
        }
        // Not while something has captured input. `self.focus` then indexes the
        // dialog's rows, not the screen's, so settling against it reads "option
        // 0" as "the top of the content" and drags the list out from under the
        // dialog. Content behind an overlay is frozen until the overlay goes.
        if interactions.captured_focus().is_none() && self.settle_scroll(&interactions) {
            return self.collect();
        }
        interactions
    }

    /// Builds, measures and collects the tree's interactions.
    ///
    /// One pass is enough for routing: [`Interactions::capture`] discards
    /// whatever was declared before it, so the table already holds only what a
    /// dialog left reachable. Painting is the case that needs more — see
    /// [`render`](Driver::render).
    fn collect(&self) -> Interactions<S::Message> {
        let mut view = self.screen.body();
        view.measure(Renderer::screen_size());

        let mut out = Interactions::new(self.focus).scrolled(self.scroll);
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
                let interactions = self.collect_settled();
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
                let interactions = self.collect_settled();
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

        // -- swipe ----------------------------------------------------------
        // A vertical swipe anywhere moves focus, which is what the C++ screens
        // do (HomeActivity). Which way is a preference, because both readings
        // are defensible: by default the swipe drags the *content*, so swiping
        // up walks down the list; with `swipe_moves_selection` it drags the
        // *selection*, so swiping up moves focus up like Button::Up.
        if self.painted {
            let direction = Input::swipe();
            if direction != SwipeDir::None {
                if let Some(message) = self.screen.on_swipe(direction) {
                    self.dispatch(message);
                    return;
                }

                // Left/Right are left alone: the back and home gestures own
                // that axis, and claiming it here would break navigation.
                let forward = if Input::swipe_moves_selection() {
                    -1
                } else {
                    1
                };
                let delta = match direction {
                    SwipeDir::Up => forward,
                    SwipeDir::Down => -forward,
                    _ => 0,
                };
                if delta != 0 {
                    let count = self.collect_settled().focusable_count();
                    if self.move_focus(delta, count) {
                        request_update();
                    }
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
                let interactions = self.collect_settled();
                if let Some(message) = focused_message(&interactions, self.focus) {
                    self.dispatch(message);
                }
            }
            Button::Up | Button::Down => {
                let count = self.collect_settled().focusable_count();
                let delta = if key == Button::Up { -1 } else { 1 };
                if self.move_focus(delta, count) {
                    request_update();
                }
            }
            Button::Left | Button::Right => {
                // Nudge whatever holds focus, so one pair of keys drives every
                // adjustable control instead of the screen wiring them to one.
                let interactions = self.collect_settled();
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

        // Settle focus before painting: a dialog appearing moves focus into it,
        // and the frame it opens on must highlight the right option.
        let capturing = self.collect_settled().captured_focus().is_some();

        let mut view = self.screen.body();
        view.measure(Renderer::screen_size());
        // Interactions run before painting so each widget learns whether it
        // holds focus and can draw itself selected. Behind a dialog it must
        // learn the opposite: a widget is told it holds focus *as it declares*,
        // so a list behind would record a focused row and keep painting the
        // highlight. Clearing the table afterwards is too late — this pass
        // ignores those declarations outright.
        let mut out = if capturing {
            Interactions::capturing(self.focus)
        } else {
            Interactions::new(self.focus)
        }
        .scrolled(self.scroll);
        view.interactions(Point::ORIGIN, &mut out);
        view.render(Point::ORIGIN);

        self.painted = true;
    }
}
