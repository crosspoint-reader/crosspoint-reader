//! What a widget declares, and what a touch or a key turns it into.
//!
//! A widget never hit-tests. It declares the regions it owns and the message
//! each produces, and the runtime resolves one frame of input against that
//! list. See [`crate::screen`] for the resolution itself.

use alloc::boxed::Box;
use alloc::vec::Vec;

use crate::geometry::Rect;
use crate::host::{Theme, ThemeMetric};

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

    /// This mask with `other`'s bits removed.
    pub const fn without(self, other: InputMask) -> InputMask {
        InputMask(self.0 & !other.0)
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

/// The value a touch at `x` represents within `track`.
///
/// Rounds to nearest, matching the `(permille * range + 500) / 1000` the C++
/// slider screens use, so dragging feels identical in both.
///
/// The inset and knob width come from the theme rather than from constants
/// here: the host draws the knob, and a copy of its dimensions would keep
/// converting touches against the old geometry the day the theme changed it.
pub fn value_at(track: Rect, x: i32, max: i32) -> i32 {
    let inset = Theme::metric(ThemeMetric::SliderSideInset);
    let knob = Theme::metric(ThemeMetric::SliderKnobWidth);

    let usable = (track.width() - inset * 2 - knob).max(1);
    let offset = (x - track.x() - inset - knob / 2).clamp(0, usable);
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
    /// Set when a view captured input, carrying the focus index it would like
    /// while it is up. `None` means nothing captured this frame.
    captured: Option<usize>,
    /// While true, declarations are ignored entirely. Set on the second pass of
    /// a frame that captures, so views behind a dialog neither take focus nor
    /// paint themselves focused — clearing them afterwards is too late, since a
    /// widget has already been told it holds focus by then.
    ignoring: bool,
    /// How far the scrolling view has been scrolled, handed down by the
    /// runtime. The view tree is rebuilt every frame, so a scroll view cannot
    /// remember this itself - it reads it here, exactly as a widget reads
    /// whether it holds focus.
    scroll: i32,
    /// The scrolling viewport and the height of what it holds, published by the
    /// scroll view during the walk so the runtime can keep focus visible.
    viewport: Option<(Rect, i32)>,
}

impl<M> Interactions<M> {
    /// A collector for a tree whose focused control is at `focus`.
    pub fn new(focus: usize) -> Self {
        Interactions {
            items: Vec::new(),
            focus,
            focusable: 0,
            captured: None,
            scroll: 0,
            viewport: None,
            ignoring: false,
        }
    }

    /// A pass that ignores everything until a view calls
    /// [`capture`](Interactions::capture).
    ///
    /// The runtime uses this once it knows the frame contains a capturing view:
    /// a first pass finds out, a second collects only what is reachable.
    pub fn capturing(focus: usize) -> Self {
        Interactions {
            items: Vec::new(),
            focus,
            focusable: 0,
            captured: None,
            scroll: 0,
            viewport: None,
            ignoring: true,
        }
    }

    /// Registers an interaction, returning whether it currently holds focus.
    pub fn declare(&mut self, rect: Rect, mask: InputMask, trigger: Trigger<M>) -> bool {
        if self.ignoring {
            return false;
        }

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
    /// Discards everything declared so far: this view is the only thing
    /// reachable while it is present.
    ///
    /// A dialog drawn over a list shares the list's tree, so without this the
    /// side buttons would walk out of the dialog and into the rows behind it —
    /// invisible on screen, and wrong. Called by a capturing view *before* it
    /// declares its own regions, since the tree is walked in draw order and
    /// everything behind has therefore already been collected.
    ///
    /// `preferred_focus` is where focus should sit while the view is up: a
    /// picker opens on the value already chosen.
    pub fn capture(&mut self, preferred_focus: usize) {
        self.items.clear();
        self.focusable = 0;
        self.captured = Some(preferred_focus);
        self.ignoring = false;
    }

    /// Starts the walk with a scroll offset the scroll view should apply.
    pub fn scrolled(mut self, offset: i32) -> Self {
        self.scroll = offset;
        self
    }

    /// The offset a scroll view should apply to its content.
    pub fn scroll(&self) -> i32 {
        self.scroll
    }

    /// Published by a scroll view: the visible band, and how tall its content
    /// is. `None` means nothing on this screen scrolls.
    pub fn set_viewport(&mut self, viewport: Rect, content_height: i32) {
        self.viewport = Some((viewport, content_height));
    }

    pub fn viewport(&self) -> Option<(Rect, i32)> {
        self.viewport
    }

    /// How many interactions have been declared so far. A container uses this
    /// to find the ones its own child added.
    pub fn len(&self) -> usize {
        self.items.len()
    }

    /// Withdraws `mask` from every interaction declared since `from` that falls
    /// outside `visible`. A scrolled-away control keeps its focus stop, so it
    /// can still be reached, but stops accepting touches aimed at whatever now
    /// occupies that part of the screen.
    pub fn restrict_outside(&mut self, from: usize, visible: Rect, mask: InputMask) {
        for item in self.items.iter_mut().skip(from) {
            if !item.rect.intersects(visible) {
                item.mask = item.mask.without(mask);
            }
        }
    }

    /// The focus index a capturing view asked for, or `None` if none captured.
    pub fn captured_focus(&self) -> Option<usize> {
        self.captured
    }

    /// Rect of the focusable interaction at `focus`, in tree order. The
    /// runtime scrolls to bring this into view.
    pub fn focused_rect(&self, focus: usize) -> Option<Rect> {
        self.items
            .iter()
            .filter(|item| item.mask.contains(InputMask::FOCUS))
            .nth(focus)
            .map(|item| item.rect)
    }

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
