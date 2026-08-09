//! A titled group of content.

use alloc::boxed::Box;
use alloc::string::String;

use crate::geometry::{Point, Rect, Size};
use crate::host::{Theme, ThemeMetric};
use crate::view::{Interactions, View};

/// A heading with content beneath it, for splitting a screen into groups.
///
/// The heading is drawn by the theme's own sub-header, so it matches the
/// section headings elsewhere in the firmware rather than being a bold `Text`
/// that merely looks similar.
///
/// ```rust,ignore
/// Section::new(heading, list)
/// ```
pub struct Section<M> {
    title: String,
    content: Box<dyn View<M>>,
    header_height: i32,
    measured: Size,
}

impl<M: 'static> Section<M> {
    pub fn new(title: impl Into<String>, content: impl View<M> + 'static) -> Self {
        Section {
            title: title.into(),
            content: Box::new(content),
            header_height: 0,
            measured: Size::ZERO,
        }
    }
}

impl<M> Section<M> {
    /// Where the content sits, below the heading. Shared so `render` and
    /// `interactions` cannot drift.
    fn content_origin(&self, origin: Point) -> Point {
        let gap = Theme::metric(ThemeMetric::VerticalSpacing);
        origin.offset(0, self.header_height + gap)
    }
}

impl<M> View<M> for Section<M> {
    fn measure(&mut self, available: Size) {
        // The theme's sub-header occupies one standard row.
        self.header_height = Theme::metric(ThemeMetric::ListRowHeight);
        let gap = Theme::metric(ThemeMetric::VerticalSpacing);

        let remaining = (available.height - self.header_height - gap).max(0);
        self.content.measure(Size::new(available.width, remaining));

        self.measured = Size::new(
            available.width,
            self.header_height + gap + self.content.size().height,
        );
    }

    fn size(&self) -> Size {
        self.measured
    }

    fn render(&self, origin: Point) {
        Theme::draw_sub_header(
            Rect::new(origin.x, origin.y, self.measured.width, self.header_height),
            &self.title,
            None,
        );

        self.content.render(self.content_origin(origin));
    }

    fn interactions(&mut self, origin: Point, out: &mut Interactions<M>) {
        let at = self.content_origin(origin);
        self.content.interactions(at, out);
    }
}
