//! A dialog offering a list of choices.

use alloc::string::String;
use alloc::vec::Vec;

use crate::geometry::{Point, Rect, Size};
use crate::host::{Renderer, Theme};
use crate::view::{InputMask, Interactions, Scrim, Trigger, View};

/// A centred dialog offering a list of options, drawn by the firmware's own
/// theme — so it is pixel-identical to the popups the C++ screens show.
///
/// It **captures input**: while one is in the tree, nothing behind it can be
/// reached, focus starts on the option already chosen, and the side buttons
/// walk its options rather than the list underneath.
///
/// Renders over whatever is already in the framebuffer without clearing, so a
/// screen draws its content first and puts the dialog last.
///
/// The screen owns whether it is open — hold that in your own state and include
/// the dialog in `body()` while it is true.
///
/// ```rust,ignore
/// Modal::picker("Refresh Frequency", ["1 page", "5 pages", "10 pages"])
///     .selected(self.current)
///     .on_select(Msg::Chose)
/// ```
pub struct Modal<M> {
    title: String,
    options: Vec<String>,
    selected: usize,
    scrim: Scrim,
    /// Set by [`on_select`](Modal::on_select); without one the dialog still
    /// captures and draws, it simply reports nothing.
    make: Option<fn(usize) -> M>,
    /// Which option held focus at the last interactions walk, so `render` can
    /// highlight it. Without this the dialog would keep painting whichever
    /// value was passed to `selected`, and the arrows would appear dead.
    focused_option: Option<usize>,
    measured: Size,
}

impl<M: Clone> Modal<M> {
    pub fn new<S: Into<String>>(
        title: impl Into<String>,
        options: impl IntoIterator<Item = S>,
    ) -> Self {
        Modal {
            title: title.into(),
            options: options.into_iter().map(Into::into).collect(),
            selected: 0,
            scrim: Scrim::None,
            make: None,
            focused_option: None,
            measured: Size::ZERO,
        }
    }

    /// Choosing one value from several — the settings-row case. Reads better
    /// than `new` at the call site, and is the same dialog.
    pub fn picker<S: Into<String>>(
        title: impl Into<String>,
        options: impl IntoIterator<Item = S>,
    ) -> Self {
        Self::new(title, options)
    }

    /// A yes/no question. The confirming choice comes first, so it is where
    /// focus opens.
    pub fn confirm<S: Into<String>>(
        title: impl Into<String>,
        choices: impl IntoIterator<Item = S>,
    ) -> Self {
        Self::new(title, choices)
    }

    /// The option to highlight, and where focus opens.
    pub fn selected(mut self, index: usize) -> Self {
        self.selected = index;
        self
    }

    /// Sends `make(index)` when an option is chosen, by touch or by Confirm.
    pub fn on_select(mut self, make: fn(usize) -> M) -> Self {
        self.make = Some(make);
        self
    }

    /// Dims what is behind the dialog. Defaults to [`Scrim::None`], matching
    /// the C++ popups, which leave the screen beneath plainly visible.
    pub fn scrim(mut self, scrim: Scrim) -> Self {
        self.scrim = scrim;
        self
    }

    pub fn len(&self) -> usize {
        self.options.len()
    }

    pub fn is_empty(&self) -> bool {
        self.options.is_empty()
    }

    /// The option the theme should highlight: whichever holds focus, falling
    /// back to the value the screen said was current.
    fn highlighted(&self) -> i32 {
        let index = self.focused_option.unwrap_or(self.selected);
        i32::try_from(index).unwrap_or(-1)
    }

    /// Screen rect of one option row. The geometry comes from C++, which
    /// already owns this maths for its own popup — deriving it again here is
    /// how the two would drift apart.
    fn row_rect(&self, index: usize) -> Option<Rect> {
        Theme::option_popup_row_rect(
            &self.title,
            &|i| self.options.get(i).map(String::as_str),
            self.options.len(),
            index,
        )
    }
}

impl<M: Clone> View<M> for Modal<M> {
    fn measure(&mut self, available: Size) {
        // The theme centres the dialog and sizes it to its content, so the
        // widget occupies the whole area as far as layout is concerned.
        self.measured = available;
    }

    fn size(&self) -> Size {
        self.measured
    }

    fn render(&self, _origin: Point) {
        if self.options.is_empty() {
            return;
        }

        if self.scrim == Scrim::Dim {
            Renderer::scrim(Renderer::screen_bounds());
        }

        Theme::draw_option_popup(
            &self.title,
            &|i| self.options.get(i).map(String::as_str),
            self.options.len(),
            self.highlighted(),
        );
    }

    fn interactions(&mut self, _origin: Point, out: &mut Interactions<M>) {
        self.focused_option = None;
        if self.options.is_empty() {
            return;
        }

        // Everything behind the dialog becomes unreachable, and focus opens on
        // the value already chosen. This happens even with no `on_select`: a
        // dialog with no outcome must still not let input through to the list.
        out.capture(self.selected);

        let Some(make) = self.make else {
            return;
        };

        for index in 0..self.options.len() {
            if let Some(rect) = self.row_rect(index)
                && out.declare(rect, InputMask::DEFAULT, Trigger::Message(make(index)))
            {
                self.focused_option = Some(index);
            }
        }
    }
}
