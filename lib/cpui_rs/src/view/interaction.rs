//! What a widget declares, and what a touch or a key turns it into.
//!
//! A widget never hit-tests. It declares the regions it owns and the message
//! each produces, and the runtime resolves one frame of input against that
//! list. See [`crate::screen`] for the resolution itself.

use alloc::boxed::Box;
use alloc::vec::Vec;

use crate::geometry::Rect;

/// Which kinds of input an interaction accepts.
///
/// This is what stops a finger resting on a button re-firing it every frame:
/// only [`InputMask::DRAG`] interactions are offered held touches, and
/// everything else acts once, on release.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct InputMask(u8);

impl InputMask {
    /// A tap: press and release inside the control.
    pub const TAP: InputMask = InputMask(1 << 0);
    /// Reachable by Up/Down, activated by Confirm.
    pub const FOCUS: InputMask = InputMask(1 << 1);
    /// Receives every frame the finger is down, with its position.
    pub const DRAG: InputMask = InputMask(1 << 2);
    /// A press held past the long-press threshold.
    pub const LONG_PRESS: InputMask = InputMask(1 << 3);
    /// Left/Right nudge this control while it holds focus. A stepper says yes;
    /// it is what lets buttons drive a value without the screen wiring keys to
    /// one particular control.
    pub const ADJUST: InputMask = InputMask(1 << 4);

    /// What an ordinary control wants: tappable, and reachable by button.
    pub const DEFAULT: InputMask = InputMask(Self::TAP.0 | Self::FOCUS.0);

    pub const fn union(self, other: InputMask) -> InputMask {
        InputMask(self.0 | other.0)
    }

    pub const fn contains(self, other: InputMask) -> bool {
        self.0 & other.0 == other.0
    }
}

impl core::ops::BitOr for InputMask {
    type Output = InputMask;

    fn bitor(self, rhs: InputMask) -> InputMask {
        self.union(rhs)
    }
}

/// What an interaction produces when it fires.
///
/// [`Trigger::Message`] covers buttons, rows and toggles: the widget knows what
/// it means, so it builds the message when the tree is built. [`Trigger::Value`]
/// is for controls whose message depends on *where* the touch landed — the
/// framework converts the position and calls the constructor, so no screen
/// re-derives slider geometry.
pub enum Trigger<M> {
    Message(M),
    Value {
        make: fn(i32) -> M,
        max: i32,
    },
    /// A relative nudge: `-1` or `+1` from Left/Right, or from a `-`/`+` glyph.
    /// Distinct from [`Trigger::Value`] because the screen adds the delta to
    /// whatever it currently holds, rather than being handed an absolute.
    Step {
        make: fn(i32) -> M,
    },
    /// A value control seen through [`ViewExt::map`].
    ///
    /// Composing two function pointers is not itself a function pointer, so a
    /// mapped value control is the one case that needs a closure. It costs one
    /// small allocation per touch frame, and only for components that actually
    /// wrap a slider — the alternative was dropping the interaction, which
    /// would silently make the control dead.
    MappedValue {
        make: Box<dyn Fn(i32) -> M>,
        max: i32,
    },
    /// A step control seen through [`ViewExt::map`]; see [`Trigger::MappedValue`].
    MappedStep {
        make: Box<dyn Fn(i32) -> M>,
    },
}

impl<M: Clone> Trigger<M> {
    /// Resolves to a message. `x` is the touch position, ignored by controls
    /// that do not depend on it.
    ///
    /// Public so a test can assert what a control *would* send without driving
    /// the whole runtime.
    pub fn resolve(&self, rect: Rect, x: i32) -> M {
        match self {
            Trigger::Message(message) => message.clone(),
            Trigger::Value { make, max } => make(value_at(rect, x, *max)),
            Trigger::MappedValue { make, max } => make(value_at(rect, x, *max)),
            // A step has no absolute reading; Confirm on one does nothing,
            // which is why `focused_message` skips it.
            Trigger::Step { make } => make(0),
            Trigger::MappedStep { make } => make(0),
        }
    }

    /// The message for a relative nudge, or `None` for a control that has no
    /// meaningful step.
    pub fn resolve_step(&self, delta: i32) -> Option<M> {
        match self {
            Trigger::Step { make } => Some(make(delta)),
            Trigger::MappedStep { make } => Some(make(delta)),
            _ => None,
        }
    }
}

/// Horizontal inset a value control reserves at each end of its track, and its
/// knob width. They live here because the framework resolves a value trigger
/// without knowing which widget produced it; [`crate::Slider`] draws to match.
pub(crate) const TRACK_INSET: i32 = 8;
pub(crate) const KNOB_WIDTH: i32 = 14;

/// The value a touch at `x` represents within `track`.
///
/// Rounds to nearest, matching the `(permille * range + 500) / 1000` the C++
/// slider screens use, so dragging feels identical in both.
pub fn value_at(track: Rect, x: i32, max: i32) -> i32 {
    let usable = (track.width() - TRACK_INSET * 2 - KNOB_WIDTH).max(1);
    let offset = (x - track.x() - TRACK_INSET - KNOB_WIDTH / 2).clamp(0, usable);
    (offset * max + usable / 2) / usable
}

/// One interactive region, as declared by the widget that owns it.
pub struct Interaction<M> {
    pub rect: Rect,
    pub mask: InputMask,
    pub trigger: Trigger<M>,
}

/// Collects a tree's interactions, telling each one whether it has focus as it
/// is declared.
///
/// Focus is an index into the focusable interactions **in tree order**. A widget
/// learns its own state from the return value of
/// [`declare`](Interactions::declare) rather than being told in a later pass,
/// so there is no third walk to keep in step with `render`.
pub struct Interactions<M> {
    items: Vec<Interaction<M>>,
    focus: usize,
    focusable: usize,
}

impl<M> Interactions<M> {
    /// A collector for a tree whose focused control is at `focus`.
    pub fn new(focus: usize) -> Self {
        Interactions {
            items: Vec::new(),
            focus,
            focusable: 0,
        }
    }

    /// Registers an interaction, returning whether it currently holds focus.
    pub fn declare(&mut self, rect: Rect, mask: InputMask, trigger: Trigger<M>) -> bool {
        let focused = mask.contains(InputMask::FOCUS) && {
            let mine = self.focusable;
            self.focusable += 1;
            mine == self.focus
        };

        self.items.push(Interaction {
            rect,
            mask,
            trigger,
        });
        focused
    }

    /// How many interactions can hold focus. The runtime wraps its cursor on
    /// this.
    pub fn focusable_count(&self) -> usize {
        self.focusable
    }

    pub fn items(&self) -> &[Interaction<M>] {
        &self.items
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    /// A collector for a sub-component, positioned so its focus numbering
    /// continues this one's. A mapped component's controls therefore sit in the
    /// parent's focus order exactly where they appear in the tree.
    pub(crate) fn child<N>(&self) -> Interactions<N> {
        Interactions::new(self.focus.saturating_sub(self.focusable))
    }

    /// Folds a sub-component's interactions in, translating its messages.
    pub(crate) fn absorb<N: 'static>(&mut self, inner: Interactions<N>, convert: fn(N) -> M)
    where
        M: 'static,
    {
        for item in inner.items {
            let trigger = match item.trigger {
                Trigger::Message(message) => Trigger::Message(convert(message)),
                Trigger::Value { make, max } => Trigger::MappedValue {
                    make: Box::new(move |value| convert(make(value))),
                    max,
                },
                Trigger::MappedValue { make, max } => Trigger::MappedValue {
                    make: Box::new(move |value| convert(make(value))),
                    max,
                },
                Trigger::Step { make } => Trigger::MappedStep {
                    make: Box::new(move |delta| convert(make(delta))),
                },
                Trigger::MappedStep { make } => Trigger::MappedStep {
                    make: Box::new(move |delta| convert(make(delta))),
                },
            };
            self.items.push(Interaction {
                rect: item.rect,
                mask: item.mask,
                trigger,
            });
        }
        self.focusable += inner.focusable;
    }
}
