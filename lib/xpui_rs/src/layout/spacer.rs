//! Flexible empty space.

use crate::geometry::{Point, Size};
use crate::view::View;

/// Absorbs the space its siblings do not use, pushing whatever follows to the
/// far end of the stack.
///
/// A stack measures spacers only after its fixed children, against just the
/// leftover space — so a spacer never pushes content past the edge.
///
/// ```rust,ignore
/// // pushes the footer to the bottom of the band
/// vstack![12; header, body, Spacer::new(), footer]
/// ```
#[derive(Default)]
pub struct Spacer {
    measured: Size,
}

impl Spacer {
    pub fn new() -> Self {
        Spacer::default()
    }
}

impl<M> View<M> for Spacer {
    fn measure(&mut self, available: Size) {
        self.measured = available;
    }

    fn size(&self) -> Size {
        self.measured
    }

    fn render(&self, _origin: Point) {
        // Nothing to draw: a spacer only occupies space.
    }

    fn is_flexible(&self) -> bool {
        true
    }

    /// A spacer records whatever it was offered, including across the stacking
    /// axis. Letting that count would make an `HStack` as tall as the space it
    /// was offered instead of as tall as its content.
    fn contributes_cross_size(&self) -> bool {
        false
    }
}
