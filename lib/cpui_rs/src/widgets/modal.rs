//! A modal option chooser.

use alloc::string::String;
use alloc::vec::Vec;

use crate::geometry::{Point, Rect, Size};
use crate::host::Theme;
use crate::view::View;

/// A centred dialog offering a list of options, drawn by the firmware theme.
///
/// Renders **over** whatever is already in the framebuffer — it does not clear.
/// A screen showing one should draw its own content first, then the modal, and
/// route input to the modal while it is open.
///
/// Stateless like the other widgets: the screen owns which option is selected
/// and whether the modal is open. [`row_rect`](Modal::row_rect) gives the hit
/// area for a row so a tap can be resolved without re-deriving the dialog
/// geometry.
///
/// ```rust,ignore
/// Modal::new("Orientation", orientations).selected(current)
/// ```
pub struct Modal {
    title: String,
    options: Vec<String>,
    selected: i32,
    measured: Size,
}

impl Modal {
    pub fn new<S: Into<String>>(
        title: impl Into<String>,
        options: impl IntoIterator<Item = S>,
    ) -> Self {
        Modal {
            title: title.into(),
            options: options.into_iter().map(Into::into).collect(),
            selected: 0,
            measured: Size::ZERO,
        }
    }

    /// Highlights an option.
    pub fn selected(mut self, index: usize) -> Self {
        self.selected = index as i32;
        self
    }

    pub fn len(&self) -> usize {
        self.options.len()
    }

    pub fn is_empty(&self) -> bool {
        self.options.is_empty()
    }

    /// Screen rect of one option row, for hit-testing a tap.
    ///
    /// `None` when the index is out of range. The geometry comes from C++,
    /// which already owns that math for its own popup.
    pub fn row_rect(&self, index: usize) -> Option<Rect> {
        Theme::option_popup_row_rect(
            &self.title,
            &|i| self.options.get(i).map(String::as_str),
            self.options.len(),
            index,
        )
    }

    /// The option whose row contains `point`, if any.
    pub fn option_at(&self, point: Point) -> Option<usize> {
        (0..self.options.len()).find(|&i| self.row_rect(i).is_some_and(|rect| rect.contains(point)))
    }
}

impl<M> View<M> for Modal {
    fn measure(&mut self, available: Size) {
        // The theme centres the dialog on screen and sizes it to its content,
        // so the modal occupies the whole area for hit-testing purposes.
        self.measured = available;
    }

    fn size(&self) -> Size {
        self.measured
    }

    fn render(&self, _origin: Point) {
        if self.options.is_empty() {
            return;
        }
        Theme::draw_option_popup(
            &self.title,
            &|i| self.options.get(i).map(String::as_str),
            self.options.len(),
            self.selected,
        );
    }
}
