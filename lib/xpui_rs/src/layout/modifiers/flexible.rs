//! Makes any view absorb the space its siblings leave.

use crate::geometry::{Point, Size};
use crate::view::{Interactions, View};

/// Makes any view absorb leftover space along its parent's stacking axis.
///
/// A slider between fixed `-` and `+` glyphs needs this: without it the slider
/// reports its natural size and the row does not fill the width.
///
/// ```rust,ignore
/// // the slider takes whatever the -/+ glyphs leave
/// hstack![gap; minus, Slider::new(v, 100).flexible(), plus]
/// ```
pub struct Flexible<V> {
    child: V,
}

impl<V> Flexible<V> {
    pub fn new(child: V) -> Self {
        Flexible { child }
    }
}

impl<V: View<M>, M> View<M> for Flexible<V> {
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
        self.child.interactions(origin, out);
    }

    fn is_flexible(&self) -> bool {
        true
    }

    /// Making a view flexible says nothing about whether it draws anything, so
    /// the child still decides.
    fn contributes_cross_size(&self) -> bool {
        self.child.contributes_cross_size()
    }
}
