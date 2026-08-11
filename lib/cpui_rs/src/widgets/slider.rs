//! A value slider.

use crate::geometry::{Point, Size};
use crate::host::{Theme, ThemeMetric};
use crate::view::{InputMask, Interactions, Trigger, View};

/// A horizontal slider showing `value` out of `max`.
///
/// Stateless by design: it draws the value it is given and never changes it.
/// The screen owns the value and adjusts it in `loop_`, which is how the
/// firmware's own interval and frontlight screens work.
///
/// ```rust,ignore
/// Slider::new(self.minutes - MIN, MAX - MIN)
/// ```
///
/// Give it [`on_change`](Slider::on_change) and the framework converts a touch
/// into a value for you, applying the same rounding the C++ screens use.
pub struct Slider<M> {
    value: i32,
    max: i32,
    /// Built when a drag or tap lands on the track; `None` leaves the slider
    /// display-only.
    make: Option<fn(i32) -> M>,
    measured: Size,
}

impl<M> Slider<M> {
    /// A slider at `value` of `max`. A `max` of zero renders empty rather than
    /// dividing by zero.
    pub fn new(value: i32, max: i32) -> Self {
        Slider {
            value,
            max,
            make: None,
            measured: Size::ZERO,
        }
    }

    /// A slider at `percent` of the way along.
    pub fn percent(percent: i32) -> Self {
        Slider::new(percent.clamp(0, 100), 100)
    }

    /// Reports drags and taps by building a message from the new value.
    ///
    /// The framework converts the touch position, so the screen never sees
    /// geometry:
    ///
    /// ```rust,ignore
    /// Slider::new(self.brightness, 100).on_change(Msg::Brightness)
    /// ```
    pub fn on_change(mut self, make: fn(i32) -> M) -> Self {
        self.make = Some(make);
        self
    }
}

impl<M> View<M> for Slider<M> {
    fn measure(&mut self, available: Size) {
        // One themed row tall, so a slider lines up with list rows beside it —
        // but never shorter than the knob the host will draw.
        let row = Theme::metric(ThemeMetric::ListRowHeight)
            .max(Theme::metric(ThemeMetric::SliderKnobHeight));
        self.measured = Size::new(available.width, row);
    }

    fn size(&self) -> Size {
        self.measured
    }

    fn interactions(&mut self, origin: Point, out: &mut Interactions<M>) {
        let Some(make) = self.make else { return };
        // Touch only: DRAG so the framework feeds held frames here and nowhere
        // else, TAP so a jab on the track jumps to that value. Button access is
        // the job of whatever owns the slider - see `Stepper`, which makes the
        // whole row one focus stop rather than three.
        out.declare(
            self.bounds(origin),
            InputMask::TAP.union(InputMask::DRAG),
            Trigger::Value {
                make,
                max: self.max,
            },
        );
    }

    fn render(&self, origin: Point) {
        if self.measured.is_empty() || self.max <= 0 {
            return;
        }

        // The host draws the track, the fill and the knob. It already owns that
        // geometry for its own screens, and deriving it again here is how the
        // two drift apart.
        Theme::draw_slider(self.bounds(origin), self.value, self.max);
    }
}
