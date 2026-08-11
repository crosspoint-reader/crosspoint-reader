//! An icon that both shows and flips a boolean.

use crate::geometry::{Point, Rect, Size};
use crate::host::{IconRef, Theme, ThemeMetric};
use crate::layout::{Frame, Modifiers};
use crate::view::{InputMask, Interactions, Trigger, View};
use crate::widgets::Icon;

/// An icon whose fill shows a boolean, and whose tap flips it.
///
/// Solid means on, outline means off — the cue the frontlight panel uses for
/// "the light is on". The glyph keeps its natural size, but the *control*
/// reserves the theme's minimum touch target, so two sitting side by side each
/// own a full finger's worth of space instead of competing for one tap.
///
/// Touch only: it deliberately stays out of the button focus order. A panel
/// meant for a finger should not grow focus stops that Up/Down has to walk
/// past. Give the screen an [`on_key`](crate::Screen::on_key) mapping if the
/// same action should also be reachable by button.
///
/// ```rust,ignore
/// IconToggle::new(IconRole::Sun, self.light_on).on_change(Msg::Light)
/// ```
pub struct IconToggle<M> {
    control: Frame<Icon>,
    on: bool,
    /// Set by [`on_change`](IconToggle::on_change); absent leaves the icon
    /// display-only, so it still draws.
    message: Option<M>,
}

impl<M: Clone> IconToggle<M> {
    /// An icon standing for `on`, drawn solid when true.
    pub fn new(icon: impl Into<IconRef>, on: bool) -> Self {
        let target = Theme::metric(ThemeMetric::MinTouchSize);
        IconToggle {
            control: Modifiers::<M>::frame(Icon::new(icon).filled(on), target, target),
            on,
            message: None,
        }
    }

    /// Sends `make(next_state)` when tapped. The framework flips the value, so
    /// the screen never writes `!self.something`.
    pub fn on_change(mut self, make: fn(bool) -> M) -> Self {
        self.message = Some(make(!self.on));
        self
    }
}

impl<M: Clone> View<M> for IconToggle<M> {
    // `Frame<Icon>` implements `View<M>` for every `M`, so these calls name the
    // one they mean. Keeping that here is the point of the widget: a screen
    // composing an icon by hand would have to write the same thing.
    fn measure(&mut self, available: Size) {
        View::<M>::measure(&mut self.control, available);
    }

    fn size(&self) -> Size {
        View::<M>::size(&self.control)
    }

    fn render(&self, origin: Point) {
        View::<M>::render(&self.control, origin);
    }

    fn interactions(&mut self, origin: Point, out: &mut Interactions<M>) {
        let Some(message) = self.message.clone() else {
            return;
        };
        // The whole control, not just the glyph: the frame is already at the
        // theme minimum, so nothing needs growing afterwards.
        out.declare(
            Rect {
                origin,
                size: self.size(),
            },
            InputMask::TAP,
            Trigger::Message(message),
        );
    }

    /// A stated touch target counts towards a row's height even beside a
    /// spacer, exactly as [`Frame`] does.
    fn contributes_cross_size(&self) -> bool {
        true
    }
}
