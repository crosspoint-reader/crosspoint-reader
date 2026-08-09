//! A value slider.

use crate::geometry::{Point, Rect, Size};
use crate::host::{Renderer, Theme, ThemeMetric};
use crate::view::{InputMask, Interactions, Trigger, View, KNOB_WIDTH, TRACK_INSET as SIDE_INSET};

/// Geometry copied from the firmware's own slider so the two look identical.
/// These are not adjustable knobs — they exist to match, not to be tuned.
const TRACK_HEIGHT: i32 = 4;
const KNOB_HEIGHT: i32 = 22;

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

    /// The knob's travel: the track inset on both sides, less the knob's own
    /// width. The knob's *left edge* moves across this, so its centre never
    /// quite reaches either end — as in the C++ slider.
    fn geometry(&self, origin: Point) -> (Rect, Rect) {
        let content_width = (self.measured.width - SIDE_INSET * 2).max(0);
        let content_x = origin.x + SIDE_INSET;

        let track = Rect::new(
            content_x,
            origin.y + (self.measured.height - TRACK_HEIGHT) / 2,
            content_width,
            TRACK_HEIGHT,
        );

        let max = self.max.max(1);
        let value = self.value.clamp(0, max);
        let travel = (content_width - KNOB_WIDTH).max(0);
        let knob = Rect::new(
            content_x + (travel * value) / max,
            origin.y + (self.measured.height - KNOB_HEIGHT) / 2,
            KNOB_WIDTH,
            KNOB_HEIGHT,
        );

        (track, knob)
    }
}

impl<M> View<M> for Slider<M> {
    fn measure(&mut self, available: Size) {
        // One themed row tall, so a slider lines up with list rows beside it.
        let row = Theme::metric(ThemeMetric::ListRowHeight).max(KNOB_HEIGHT);
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

        let (track, knob) = self.geometry(origin);

        // Dithered track, solid fill to the value, then the knob over both.
        Renderer::fill_rect_dither(track, true);

        let filled_width = (track.width() * self.value.clamp(0, self.max)) / self.max.max(1);
        if filled_width > 0 {
            Renderer::fill_rect(
                Rect::new(track.x(), track.y(), filled_width, track.height()),
                true,
            );
        }

        Renderer::fill_rect(knob, true);
        Renderer::stroke_rect(knob);
    }
}
