//! Wrappers that add behaviour to any view.
//!
//! All are reached through the [`Modifiers`] extension trait rather than
//! constructed directly, so a tree reads as a chain:
//!
//! ```rust,ignore
//! Text::new("-").frame(row, row).on_tap(Msg::Decrement)
//! Slider::new(value, 100).flexible()
//! ```

mod flexible;
mod frame;
mod tappable;

pub use flexible::Flexible;
pub use frame::Frame;
pub use tappable::Tappable;

use crate::view::{InputMask, View};

/// Chainable modifiers, available on every view.
pub trait Modifiers<M>: View<M> + Sized {
    /// Report a touch on this view as `message`. Also reachable by Up/Down and
    /// fired by Confirm, so a finger and a button produce the same message.
    fn on_tap(self, message: M) -> Tappable<Self, M> {
        Tappable::new(self, message)
    }

    /// Report a touch as `message`, **without** joining the focus order.
    ///
    /// For controls that are meant for a finger and would otherwise add a stop
    /// that buttons have no way to activate.
    fn on_touch(self, message: M) -> Tappable<Self, M> {
        Tappable::new(self, message).accepting(InputMask::TAP)
    }

    /// Report a press held past the long-press threshold as `message`, in
    /// addition to an ordinary tap.
    fn on_long_press(self, message: M) -> Tappable<Self, M> {
        Tappable::new(self, message).accepting(InputMask::DEFAULT.union(InputMask::LONG_PRESS))
    }

    /// Absorb the space fixed-size siblings leave along the stacking axis.
    fn flexible(self) -> Flexible<Self> {
        Flexible::new(self)
    }

    /// Fix this view's size, centring it in the frame. Either axis may be zero
    /// to stay natural.
    fn frame(self, width: i32, height: i32) -> Frame<Self> {
        Frame::new(self).width(width).height(height)
    }
}

impl<M, V: View<M> + Sized> Modifiers<M> for V {}
