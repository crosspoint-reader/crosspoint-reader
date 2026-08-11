//! A boolean setting.

use alloc::string::String;

use crate::geometry::{Point, Size};
use crate::view::{Interactions, View};
use crate::widgets::{List, ListRow};

/// A row whose value reads as one of two words.
///
/// This is what a toggle *is* in this firmware — no C++ screen draws a switch
/// graphic, and one here would look foreign beside them. It renders through the
/// theme's list, so it is identical whether it stands alone or sits in a
/// [`List`].
///
/// ```rust,ignore
/// Toggle::new("Hyphenation", self.on, "On", "Off")
///     .on_change(Msg::Hyphenation)
/// ```
pub struct Toggle<M> {
    list: List<M>,
    on: bool,
    row: Option<ListRow<M>>,
}

impl<M: Clone> Toggle<M> {
    pub fn new(
        label: impl Into<String>,
        on: bool,
        on_label: impl Into<String>,
        off_label: impl Into<String>,
    ) -> Self {
        Toggle {
            list: List::new(),
            on,
            row: Some(ListRow::toggle(label, on, on_label, off_label)),
        }
    }

    /// Sends `make(next_state)` when tapped or confirmed. The framework flips
    /// the value, so the screen never writes `!self.something`.
    pub fn on_change(mut self, make: fn(bool) -> M) -> Self {
        let next = !self.on;
        if let Some(row) = self.row.take() {
            self.list = List::new().push(row.on_tap(make(next)));
        }
        self
    }

    /// Consumes the builder into the row, for putting several in one [`List`].
    pub fn into_row(mut self) -> Option<ListRow<M>> {
        self.row.take()
    }

    /// Materialises the row when no `on_change` was given, so a display-only
    /// toggle still draws.
    fn realise(&mut self) {
        if let Some(row) = self.row.take() {
            self.list = List::new().push(row);
        }
    }
}

impl<M: Clone> View<M> for Toggle<M> {
    fn measure(&mut self, available: Size) {
        self.realise();
        self.list.measure(available);
    }

    fn size(&self) -> Size {
        self.list.size()
    }

    fn render(&self, origin: Point) {
        self.list.render(origin);
    }

    fn interactions(&mut self, origin: Point, out: &mut Interactions<M>) {
        self.list.interactions(origin, out);
    }
}
