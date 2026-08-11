//! Stacks: lay children out along one axis.
//!
//! The axis and cross-alignment live in [`axis`]; this is the layout itself.
//!
//! [`VStack`] and [`HStack`] are the same algorithm with the axes swapped, so
//! they share one implementation. Both do a two-pass layout: fixed-size
//! children are measured first, then whatever space is left is split between
//! the flexible ones. Without that split, a [`Spacer`](super::Spacer) would be
//! measured against the full screen and push its siblings off the bottom.

use alloc::boxed::Box;
use alloc::vec::Vec;

mod axis;

pub use axis::Alignment;
use axis::Axis;

use crate::geometry::{Point, Size};
use crate::view::{Interactions, View};

struct Stack<M> {
    children: Vec<Box<dyn View<M>>>,
    spacing: i32,
    axis: Axis,
    alignment: Alignment,
    measured: Size,
}

impl<M> Stack<M> {
    fn new(axis: Axis, spacing: i32) -> Self {
        Stack {
            children: Vec::new(),
            spacing,
            axis,
            alignment: Alignment::Start,
            measured: Size::ZERO,
        }
    }

    fn total_spacing(&self) -> i32 {
        self.spacing * (self.children.len().saturating_sub(1)) as i32
    }

    fn measure(&mut self, available: Size) {
        let axis = self.axis;
        let available_main = axis.main(available);
        let available_cross = axis.cross(available);
        let spacing = self.total_spacing();

        // Pass 1: fixed children take what they need.
        let mut fixed_main = 0;
        let mut flexible = 0;
        for child in &mut self.children {
            if child.is_flexible() {
                flexible += 1;
            } else {
                child.measure(axis.size(available_main, available_cross));
                fixed_main += axis.main(child.size());
            }
        }

        // Pass 2: flexible children divide what is left.
        if flexible > 0 {
            let leftover = (available_main - fixed_main - spacing).max(0);
            let share = leftover / flexible;
            for child in self.children.iter_mut().filter(|c| c.is_flexible()) {
                child.measure(axis.size(share, available_cross));
            }
        }

        let main: i32 = self
            .children
            .iter()
            .map(|child| axis.main(child.size()))
            .sum();
        // Spacers are excluded: they record the full cross extent they were
        // offered, so counting them would size the stack to the space
        // available rather than to its content.
        let cross = self
            .children
            .iter()
            .filter(|child| child.contributes_cross_size())
            .map(|child| axis.cross(child.size()))
            .max()
            .unwrap_or(0);

        self.measured = axis.size(main + spacing, cross);
    }

    /// The child origin `render` and `interactions` both use, so the one place
    /// that derives a position is shared rather than written twice.
    fn place(&self, cursor: Point, child: &dyn View<M>) -> Point {
        Stack::place_at(cursor, child, self.axis, self.alignment, self.measured)
    }

    /// `place` without borrowing the whole stack, so the `&mut` interaction
    /// walk can use the very same arithmetic.
    fn place_at(
        cursor: Point,
        child: &dyn View<M>,
        axis: Axis,
        alignment: Alignment,
        measured: Size,
    ) -> Point {
        let slack = axis.cross(measured) - axis.cross(child.size());
        let offset = match alignment {
            Alignment::Start => 0,
            Alignment::Center => slack / 2,
            Alignment::End => slack,
        }
        .max(0);
        axis.advance_cross(cursor, offset)
    }

    fn render(&self, origin: Point) {
        let mut cursor = origin;
        for child in &self.children {
            child.render(self.place(cursor, child.as_ref()));
            cursor = self
                .axis
                .advance(cursor, self.axis.main(child.size()) + self.spacing);
        }
    }

    /// Walks children in the same order and with the same cursor arithmetic as
    /// `render`, via the shared [`place`](Stack::place). The two must stay
    /// identical or touches land on the wrong control.
    fn interactions(&mut self, origin: Point, out: &mut Interactions<M>) {
        let mut cursor = origin;
        for child in &mut self.children {
            let at = Stack::place_at(
                cursor,
                child.as_ref(),
                self.axis,
                self.alignment,
                self.measured,
            );
            child.interactions(at, out);
            cursor = self
                .axis
                .advance(cursor, self.axis.main(child.size()) + self.spacing);
        }
    }
}

/// Lays children out top to bottom.
///
/// ```rust,ignore
/// VStack::new(20)
///     .push(Text::new("Title").boxed())
///     .push(Spacer::new().boxed())
///     .push(Text::new("Footer").boxed())
/// ```
pub struct VStack<M>(Stack<M>);

/// Lays children out left to right.
pub struct HStack<M>(Stack<M>);

macro_rules! stack_impl {
    ($name:ident, $axis:expr, $doc:literal) => {
        impl<M: 'static> $name<M> {
            #[doc = $doc]
            pub fn new(spacing: i32) -> Self {
                $name(Stack::new($axis, spacing))
            }

            /// Positions children across the stacking axis. Defaults to
            /// [`Alignment::Start`].
            pub fn align(mut self, alignment: Alignment) -> Self {
                self.0.alignment = alignment;
                self
            }

            /// Appends a child.
            ///
            /// Takes any view by value and boxes it internally, so callers
            /// never write `Box::new` to build a tree.
            pub fn push(mut self, child: impl View<M> + 'static) -> Self {
                self.0.children.push(Box::new(child));
                self
            }

            /// Appends a child only when `condition` holds, so callers can
            /// build conditional trees without wrapping the whole chain in an
            /// `if`.
            pub fn push_if(self, condition: bool, child: impl View<M> + 'static) -> Self {
                if condition {
                    self.push(child)
                } else {
                    self
                }
            }

            /// Appends a child when there is one.
            pub fn push_some(self, child: Option<impl View<M> + 'static>) -> Self {
                match child {
                    Some(child) => self.push(child),
                    None => self,
                }
            }

            /// Appends every view an iterator yields, for lists built at run
            /// time. The item type is uniform; mix types by boxing to
            /// `Box<dyn View>` first, which is itself a view.
            pub fn extend<V: View<M> + 'static>(
                mut self,
                children: impl IntoIterator<Item = V>,
            ) -> Self {
                self.0.children.extend(
                    children
                        .into_iter()
                        .map(|c| Box::new(c) as Box<dyn View<M>>),
                );
                self
            }
        }

        impl<M> View<M> for $name<M> {
            fn measure(&mut self, available: Size) {
                self.0.measure(available);
            }

            fn size(&self) -> Size {
                self.0.measured
            }

            fn render(&self, origin: Point) {
                self.0.render(origin);
            }

            fn interactions(&mut self, origin: Point, out: &mut Interactions<M>) {
                self.0.interactions(origin, out);
            }
        }
    };
}

stack_impl!(
    VStack,
    Axis::Vertical,
    "A vertical stack with `spacing` pixels between children."
);
stack_impl!(
    HStack,
    Axis::Horizontal,
    "A horizontal stack with `spacing` pixels between children."
);
