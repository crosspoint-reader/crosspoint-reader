//! Turning one frame of input into a message.
//!
//! Pure functions over the interactions a tree declared. Kept apart from the
//! runtime so the *policy* — which control a touch resolves to, what Confirm
//! fires, which control Left/Right nudge — can be read and tested without the
//! lifecycle around it.

use crate::Point;
use crate::view::{InputMask, Interactions, Trigger};

/// The message from the first interaction under `point` that accepts `kind`.
///
/// Later declarations win: a container declares its own region before its
/// children in a `Tappable`, so scanning backwards resolves to the innermost
/// control.
pub(crate) fn resolve<M: Clone>(
    interactions: &Interactions<M>,
    point: Point,
    kind: InputMask,
) -> Option<M> {
    interactions
        .items()
        .iter()
        .rev()
        .find(|item| item.mask.contains(kind) && item.rect.contains(point))
        .map(|item| item.trigger.resolve(item.rect, point.x))
}

/// The focused interaction, if any.
pub(crate) fn focused<M>(
    interactions: &Interactions<M>,
    focus: usize,
) -> Option<&crate::view::Interaction<M>> {
    interactions
        .items()
        .iter()
        .filter(|item| item.mask.contains(InputMask::FOCUS))
        .nth(focus)
}

/// A relative nudge for the focused control, for Left/Right.
pub(crate) fn focused_step<M: Clone>(
    interactions: &Interactions<M>,
    focus: usize,
    delta: i32,
) -> Option<M> {
    let item = focused(interactions, focus)?;
    if !item.mask.contains(InputMask::ADJUST) {
        return None;
    }
    item.trigger.resolve_step(delta)
}

/// The focused interaction's message, for Confirm.
pub(crate) fn focused_message<M: Clone>(interactions: &Interactions<M>, focus: usize) -> Option<M> {
    let item = focused(interactions, focus)?;

    // Confirm on an adjustable control does nothing: there is no absolute
    // reading to commit, and Left/Right are what drive it.
    if matches!(
        item.trigger,
        Trigger::Step { .. } | Trigger::MappedStep { .. }
    ) {
        return None;
    }

    let x = item.rect.x() + item.rect.width() / 2;
    Some(item.trigger.resolve(item.rect, x))
}

// -- driving a screen -------------------------------------------------------
//
// `Screen` uses `impl View` in return position, so it is deliberately never a
// trait object. `Runtime<S>` erases the screen type behind `Driver` instead,
// which is what a host dispatches through.
