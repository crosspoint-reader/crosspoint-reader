//! A selectable list drawn by the host's theme.

mod row;

pub use row::ListRow;

use alloc::vec::Vec;

use crate::geometry::{Point, Rect, Size};
use crate::host::{Theme, ThemeMetric};
use crate::view::{InputMask, Interactions, Trigger, View};

/// A themed, selectable list filling the space it is given.
///
/// The theme owns row height, the selection highlight and pagination, so this
/// looks and behaves exactly like the C++ list screens.
///
/// ```rust,ignore
/// List::new()
///     .push(ListRow::new("Frontlight").value("On").on_tap(Msg::Light))
///     .push(ListRow::new("Free Heap").value(free))
/// ```
pub struct List<M> {
    rows: Vec<ListRow<M>>,
    /// Explicit override; -1 means "let focus decide".
    selected: i32,
    /// Which row held focus at the last interactions walk, so `render` can
    /// highlight it without the screen tracking an index.
    focused_row: Option<usize>,
    measured: Size,
}

impl<M> Default for List<M> {
    fn default() -> Self {
        List::new()
    }
}

impl<M> List<M> {
    pub fn new() -> Self {
        List {
            rows: Vec::new(),
            selected: -1,
            focused_row: None,
            measured: Size::ZERO,
        }
    }

    pub fn push(mut self, row: ListRow<M>) -> Self {
        self.rows.push(row);
        self
    }

    pub fn extend(mut self, rows: impl IntoIterator<Item = ListRow<M>>) -> Self {
        self.rows.extend(rows);
        self
    }

    /// Highlights a row explicitly, overriding the framework's focus.
    ///
    /// Rarely needed: a list whose rows carry messages is highlighted by
    /// whichever row currently holds focus. Out-of-range values highlight
    /// nothing.
    pub fn selected(mut self, index: usize) -> Self {
        self.selected = i32::try_from(index).unwrap_or(-1);
        self
    }

    pub fn len(&self) -> usize {
        self.rows.len()
    }

    pub fn is_empty(&self) -> bool {
        self.rows.is_empty()
    }
}

impl<M> List<M> {
    /// Whether any row carries a subtitle, which makes the theme use its
    /// taller two-line row.
    fn has_subtitle(&self) -> bool {
        self.rows.iter().any(|row| row.subtitle.is_some())
    }

    /// Height of one row, as the active theme draws it.
    fn row_height(&self) -> i32 {
        Theme::metric(if self.has_subtitle() {
            ThemeMetric::ListRowHeightWithSubtitle
        } else {
            ThemeMetric::ListRowHeight
        })
    }

    /// Distance from the top of one row to the top of the next, gap included.
    fn row_pitch(&self) -> i32 {
        self.row_height() + Theme::metric(ThemeMetric::ListRowGap)
    }
}

impl<M> List<M> {
    /// The row the theme should draw as selected: whichever holds focus, or
    /// the explicit override when a screen set one.
    fn highlighted(&self) -> i32 {
        match self.focused_row {
            Some(index) => i32::try_from(index).unwrap_or(-1),
            None => self.selected,
        }
    }
}

impl<M: Clone> View<M> for List<M> {
    fn measure(&mut self, available: Size) {
        // Only as tall as the rows need, capped by what is offered. Claiming
        // the whole band would stop several lists sharing one screen, which is
        // what a sectioned screen is made of.
        let rows = self.rows.len() as i32;
        let needed = (self.row_height() * rows
            + Theme::metric(ThemeMetric::ListRowGap) * (rows - 1).max(0))
        .max(0);
        self.measured = Size::new(available.width, needed.min(available.height));
    }

    fn size(&self) -> Size {
        self.measured
    }

    /// Declares one interaction per interactive row, in order, so focus moves
    /// down the list exactly as it reads. Rows are uniform, so each rect is the
    /// same row height the theme draws with.
    fn interactions(&mut self, origin: Point, out: &mut Interactions<M>) {
        self.focused_row = None;
        let height = self.row_height().max(1);
        let pitch = self.row_pitch().max(1);
        let width = self.measured.width;

        for (index, row) in self.rows.iter().enumerate() {
            let Some(message) = row.message.clone() else {
                continue;
            };
            let rect = Rect::new(origin.x, origin.y + index as i32 * pitch, width, height);
            if out.declare(rect, InputMask::DEFAULT, Trigger::Message(message)) {
                self.focused_row = Some(index);
            }
        }
    }

    fn render(&self, origin: Point) {
        Theme::draw_list(
            Rect {
                origin,
                size: self.measured,
            },
            self.rows.len(),
            self.highlighted(),
            &|index, field| self.rows.get(index).and_then(|row| row.field(field)),
        );
    }
}
