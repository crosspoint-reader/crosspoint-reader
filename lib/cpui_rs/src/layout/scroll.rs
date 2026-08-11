//! A view that shows a window onto content taller than itself.

use alloc::boxed::Box;

use crate::geometry::{Point, Rect, Size};
use crate::host::{Renderer, Theme};
use crate::view::{InputMask, Interactions, View};

/// Shows as much of its content as fits, and lets the rest be scrolled to.
///
/// Wrap a screen's content in one and everything below the fold becomes
/// reachable — by swipe on a touch panel, by Up/Down on a button one. The
/// runtime scrolls to keep whatever holds focus on screen, so a screen never
/// tracks a scroll position itself.
///
/// ```rust,ignore
/// NavigationScreen::new(ScrollView::new(vstack![8;
///     Section::new("Memory", self.memory_rows()),
///     Section::new("Scrolling", self.long_list()),
/// ]))
/// ```
///
/// Content is drawn under a clip, so what overflows is discarded rather than
/// painted over the header and the button hints.
///
/// **One per screen.** The runtime owns a single scroll offset, so nesting two
/// would give both the same one.
pub struct ScrollView<M> {
    content: Box<dyn View<M>>,
    /// The band this view occupies, from `measure`.
    measured: Size,
    /// How tall the content wants to be, which is what makes scrolling
    /// necessary and bounds how far it can go.
    content_height: i32,
    /// Handed down by the runtime through `interactions`, and remembered for
    /// `render` — which is walked immediately afterwards, the same way `List`
    /// learns which row holds focus.
    offset: i32,
}

impl<M: Clone + 'static> ScrollView<M> {
    pub fn new(content: impl View<M> + 'static) -> Self {
        ScrollView {
            content: Box::new(content),
            measured: Size::ZERO,
            content_height: 0,
            offset: 0,
        }
    }

    /// The furthest the content can be scrolled: everything below the fold, and
    /// no further, so the last item lands at the bottom rather than scrolling
    /// off into blank space.
    fn max_offset(&self) -> i32 {
        (self.content_height - self.measured.height).max(0)
    }

    /// The band the clip and the indicator use: the full panel width at this
    /// view's vertical extent.
    ///
    /// Wider than the content on purpose. The content keeps whatever side
    /// padding the screen gave it, and the indicator hangs at the panel's edge
    /// in that margin - which is how FreeInkUI arranges its own lists, and
    /// where the eye expects it.
    fn band(&self, origin: Point) -> Rect {
        Rect::new(
            0,
            origin.y,
            Renderer::screen_size().width,
            self.measured.height,
        )
    }
}

impl<M: Clone + 'static> View<M> for ScrollView<M> {
    fn measure(&mut self, available: Size) {
        // The content is measured against its natural height rather than the
        // band: asking it to fit is what squeezed rows out before there was a
        // scroll view to hold them.
        self.content.measure(Size::new(available.width, i32::MAX));
        self.content_height = self.content.size().height;
        self.measured = available;
    }

    fn size(&self) -> Size {
        self.measured
    }

    fn interactions(&mut self, origin: Point, out: &mut Interactions<M>) {
        self.offset = out.scroll().clamp(0, self.max_offset());

        let viewport = self.band(origin);
        out.set_viewport(viewport, self.content_height);

        // Declared at their scrolled positions, so a touch lands on what the
        // eye sees.
        let shifted = Point::new(origin.x, origin.y - self.offset);
        let before = out.len();
        self.content.interactions(shifted, out);

        // Anything scrolled out of sight keeps its focus stop — that is how it
        // can be reached at all — but must not take a touch: its rect now lies
        // over the chrome, where a tap means something else entirely.
        out.restrict_outside(before, viewport, InputMask::TAP.union(InputMask::DRAG));
    }

    fn render(&self, origin: Point) {
        let viewport = self.band(origin);

        Renderer::clip(viewport);
        self.content
            .render(Point::new(origin.x, origin.y - self.offset));
        Renderer::clear_clip();

        // After the clip is lifted: the indicator sits at the viewport's edge
        // and would otherwise be trimmed by it. The host draws nothing when
        // the content already fits.
        Theme::draw_scroll_indicator(
            viewport,
            self.content_height,
            self.measured.height,
            self.offset,
        );
    }
}
