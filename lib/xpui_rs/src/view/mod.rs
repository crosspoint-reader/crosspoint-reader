//! The `View` trait every UI element implements.

mod interaction;

pub use interaction::{InputMask, Interaction, Interactions, Trigger, value_at};

use alloc::boxed::Box;

use crate::geometry::{Point, Rect, Size};

/// A node in the UI tree.
///
/// Generic over the screen's message type: a widget carries values of `M` and
/// hands them back when touched, so a screen matches on its own enum instead of
/// translating opaque ids. Layout runs in three passes:
///
/// 1. [`measure`](View::measure) — records the size this view wants.
/// 2. [`interactions`](View::interactions) — declares interactive regions at
///    their final positions, and learns which one has focus.
/// 3. [`render`](View::render) — paints.
///
/// Passes 2 and 3 walk the same geometry. A container must place children
/// identically in both, or a touch lands on the control next door.
pub trait View<M> {
    /// Compute and store the size this view wants within `available`.
    fn measure(&mut self, available: Size);

    /// The size recorded by the most recent [`measure`](View::measure).
    fn size(&self) -> Size;

    /// Paint with the view's top-left corner at `origin`.
    fn render(&self, origin: Point);

    /// Declare any interactive regions, given this view sits at `origin`.
    /// Non-interactive leaves keep the default and declare nothing.
    fn interactions(&mut self, origin: Point, out: &mut Interactions<M>) {
        let _ = (origin, out);
    }

    /// Whether this view absorbs leftover space along its parent's stacking
    /// axis. Flexible views are measured in a second pass, against only what
    /// the fixed-size siblings left behind.
    fn is_flexible(&self) -> bool {
        false
    }

    /// Whether this view's measured size counts towards its parent's extent
    /// *across* the stacking axis.
    ///
    /// Only [`Spacer`](crate::Spacer) says no. A spacer is offered the full
    /// cross extent and records it, so counting it would make an `HStack` as
    /// tall as the space it was offered rather than as tall as its content.
    fn contributes_cross_size(&self) -> bool {
        true
    }

    /// This view's own bounds when placed at `origin`.
    fn bounds(&self, origin: Point) -> Rect {
        Rect {
            origin,
            size: self.size(),
        }
    }
}

/// A boxed view is itself a view — including `Box<dyn View<M>>`, so views of
/// different shapes can be collected into one sequence.
impl<M, V: View<M> + ?Sized> View<M> for Box<V> {
    fn measure(&mut self, available: Size) {
        (**self).measure(available)
    }

    fn size(&self) -> Size {
        (**self).size()
    }

    fn render(&self, origin: Point) {
        (**self).render(origin)
    }

    fn interactions(&mut self, origin: Point, out: &mut Interactions<M>) {
        (**self).interactions(origin, out)
    }

    fn is_flexible(&self) -> bool {
        (**self).is_flexible()
    }

    fn contributes_cross_size(&self) -> bool {
        (**self).contributes_cross_size()
    }
}

/// A component's view, with its messages translated into its parent's.
///
/// Built by [`ViewExt::map`].
pub struct Mapped<V, N, M> {
    inner: V,
    convert: fn(N) -> M,
}

impl<V, N, M> View<M> for Mapped<V, N, M>
where
    V: View<N>,
    N: 'static,
    M: 'static,
{
    fn measure(&mut self, available: Size) {
        self.inner.measure(available)
    }

    fn size(&self) -> Size {
        self.inner.size()
    }

    fn render(&self, origin: Point) {
        self.inner.render(origin)
    }

    fn interactions(&mut self, origin: Point, out: &mut Interactions<M>) {
        let mut inner = out.child();
        self.inner.interactions(origin, &mut inner);
        out.absorb(inner, self.convert);
    }

    fn is_flexible(&self) -> bool {
        self.inner.is_flexible()
    }

    fn contributes_cross_size(&self) -> bool {
        self.inner.contributes_cross_size()
    }
}

/// Chainable helpers available on every view.
pub trait ViewExt<M>: View<M> + Sized {
    /// Erase this view's type, for collecting views of different shapes.
    fn boxed(self) -> Box<dyn View<M>>
    where
        Self: 'static,
    {
        Box::new(self)
    }

    /// Translate this view's messages into another type.
    ///
    /// This is what lets a component own its state and its own message enum
    /// while still composing into a screen:
    ///
    /// ```rust,ignore
    /// vstack![ self.units.view(free).map(Msg::Units) ]
    /// ```
    fn map<P>(self, convert: fn(M) -> P) -> Mapped<Self, M, P> {
        Mapped {
            inner: self,
            convert,
        }
    }
}

impl<M, V: View<M> + Sized> ViewExt<M> for V {}

/// How a view treats whatever is already on the panel behind it.
///
/// Used by anything that paints over existing content — an overlay panel, a
/// dialog — so the choice reads the same wherever it appears.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub enum Scrim {
    /// Left exactly as it was.
    #[default]
    None,
    /// Darkened, so the panel reads as the foreground. What is behind stays
    /// legible — roughly half its pixels survive.
    Dim,
}
