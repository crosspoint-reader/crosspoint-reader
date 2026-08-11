//! A run of text on one line.

use alloc::string::String;

use crate::geometry::{Point, Size};
use crate::host::{Font, Renderer};
use crate::view::View;

/// Draws a single line of text.
///
/// The string is converted to a NUL-terminated buffer once, at construction,
/// rather than on every frame — the render path allocating per draw is exactly
/// the sort of heap churn that fragments a 380 KB heap.
///
/// Size comes from the firmware's own font metrics. Estimating it instead is
/// what previously let content drift past the bottom of the screen.
///
/// ```rust,ignore
/// Text::new("Battery")
/// Text::new(format!("{percent}%")).bold()
/// Text::new(label).font(Font::ui_small())
/// ```
pub struct Text {
    content: String,
    font: Font,
    measured: Size,
}

impl Text {
    /// Text in the default interface font.
    ///
    /// A string containing an interior NUL renders as empty, since it cannot
    /// be passed to the C++ renderer.
    pub fn new(content: impl Into<String>) -> Self {
        Text {
            content: content.into(),
            font: Font::ui(),
            measured: Size::ZERO,
        }
    }

    /// Draws in `font` instead of the default.
    pub fn font(mut self, font: Font) -> Self {
        self.font = font;
        self
    }

    /// Draws bold.
    pub fn bold(mut self) -> Self {
        self.font = self.font.bold();
        self
    }

    /// Draws italic.
    pub fn italic(mut self) -> Self {
        self.font = self.font.italic();
        self
    }
}

impl<M> View<M> for Text {
    fn measure(&mut self, _available: Size) {
        self.measured = Size::new(self.font.text_width(&self.content), self.font.line_height());
    }

    fn size(&self) -> Size {
        self.measured
    }

    fn render(&self, origin: Point) {
        Renderer::draw_text(origin, &self.content, self.font.id(), self.font.style());
    }
}
