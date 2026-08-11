//! Insets a child from its parent's edges.

use alloc::boxed::Box;

use crate::geometry::{Insets, Point, Size};
use crate::view::{Interactions, View};

/// Surrounds a child with empty space.
///
/// ```rust,ignore
/// Padding::all(content, 12)
/// Padding::symmetric(content, 16, 8)
/// ```
pub struct Padding<M> {
    child: Box<dyn View<M>>,
    insets: Insets,
    measured: Size,
}

impl<M> Padding<M> {
    /// The same inset on every edge.
    pub fn all(child: impl View<M> + 'static, inset: i32) -> Self
    where
        M: 'static,
    {
        Padding::new(child, Insets::all(inset))
    }

    /// Independent horizontal and vertical insets.
    pub fn symmetric(child: impl View<M> + 'static, horizontal: i32, vertical: i32) -> Self
    where
        M: 'static,
    {
        Padding::new(child, Insets::symmetric(horizontal, vertical))
    }

    pub fn new(child: impl View<M> + 'static, insets: Insets) -> Self
    where
        M: 'static,
    {
        Padding {
            child: Box::new(child),
            insets,
            measured: Size::ZERO,
        }
    }

    /// Where the child sits. Shared so `render` and `interactions` cannot drift.
    fn child_origin(&self, origin: Point) -> Point {
        origin.offset(self.insets.left, self.insets.top)
    }
}

impl<M> View<M> for Padding<M> {
    fn measure(&mut self, available: Size) {
        let insets = self.insets;
        self.child
            .measure(available.shrink(insets.horizontal(), insets.vertical()));

        let child = self.child.size();
        self.measured = Size::new(
            child.width + insets.horizontal(),
            child.height + insets.vertical(),
        );
    }

    fn size(&self) -> Size {
        self.measured
    }

    fn render(&self, origin: Point) {
        self.child.render(self.child_origin(origin));
    }

    fn interactions(&mut self, origin: Point, out: &mut Interactions<M>) {
        let at = self.child_origin(origin);
        self.child.interactions(at, out);
    }

    /// Padding is as flexible as what it wraps, so wrapping a spacer in padding
    /// keeps it flexible.
    fn is_flexible(&self) -> bool {
        self.child.is_flexible()
    }

    /// A padded spacer is still a spacer; the insets alone are not content.
    fn contributes_cross_size(&self) -> bool {
        self.child.contributes_cross_size()
    }
}
