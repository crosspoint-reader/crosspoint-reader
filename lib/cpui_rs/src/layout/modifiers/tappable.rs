//! Makes any view report a touch as a message.

use crate::geometry::{Point, Rect, Size};
use crate::host::{Theme, ThemeMetric};
use crate::view::{InputMask, Interactions, Trigger, View};

/// Makes any view report a touch as a message.
///
/// The visible size is unchanged; only the *hit* area grows, to at least the
/// theme's minimum touch target. A 6px "−" glyph is a legitimate control but an
/// impossible thing to hit with a finger, so the target is widened around it —
/// the same reasoning as `ensureMinTouchRect` in the C++ SDK.
///
/// ```rust,ignore
/// Text::new("-").on_tap(Msg::Decrement)
/// row.on_long_press(Msg::ShowContextMenu)
/// icon.on_touch(Msg::Toggle)   // touch only, stays out of the focus order
/// ```
pub struct Tappable<V, M> {
    child: V,
    message: M,
    mask: InputMask,
}

impl<V, M> Tappable<V, M> {
    pub fn new(child: V, message: M) -> Self {
        Tappable {
            child,
            message,
            mask: InputMask::DEFAULT,
        }
    }

    /// Replaces the accepted input kinds. Defaults to [`InputMask::DEFAULT`] —
    /// tap plus button focus.
    pub fn accepting(mut self, mask: InputMask) -> Self {
        self.mask = mask;
        self
    }
}

impl<V: View<M>, M: Clone> View<M> for Tappable<V, M> {
    fn measure(&mut self, available: Size) {
        self.child.measure(available);
    }

    fn size(&self) -> Size {
        self.child.size()
    }

    fn render(&self, origin: Point) {
        self.child.render(origin);
    }

    fn interactions(&mut self, origin: Point, out: &mut Interactions<M>) {
        // The child goes first, so wrapping a container still resolves to the
        // innermost control when both cover the point.
        self.child.interactions(origin, out);

        // Grow to the theme's minimum touch target, centred on what was drawn.
        let bounds = self.child.bounds(origin);
        let minimum = Theme::metric(ThemeMetric::MinTouchSize);
        let grow_x = (minimum - bounds.width()).max(0) / 2;
        let grow_y = (minimum - bounds.height()).max(0) / 2;
        let area = Rect::new(
            bounds.x() - grow_x,
            bounds.y() - grow_y,
            bounds.width() + grow_x * 2,
            bounds.height() + grow_y * 2,
        );

        out.declare(area, self.mask, Trigger::Message(self.message.clone()));
    }

    fn is_flexible(&self) -> bool {
        self.child.is_flexible()
    }

    fn contributes_cross_size(&self) -> bool {
        self.child.contributes_cross_size()
    }
}
