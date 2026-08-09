//! A drop-down panel over the current screen.

use alloc::boxed::Box;
use alloc::string::String;

use crate::geometry::{Point, Rect, Size};
use crate::host::{Renderer, ScreenChrome, Theme, ThemeMetric};
use crate::view::{InputMask, Interactions, Trigger, View};

/// Thickness of the rule marking the panel's bottom edge.
const EDGE_THICKNESS: i32 = 2;

/// How the screen showing below an [`OverlayPanel`] is treated.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub enum Scrim {
    /// Left exactly as it was.
    #[default]
    None,
    /// Darkened, so the panel reads as the foreground. What is behind stays
    /// legible — roughly half its pixels survive.
    Dim,
}

/// The root view for a panel that drops over whatever is already on screen.
///
/// Unlike [`NavigationScreen`](super::NavigationScreen) this deliberately
/// leaves the framebuffer below it intact, so the screen it opened over stays
/// visible and a tap down there can dismiss it. The owning activity must
/// therefore **not** clear before rendering.
///
/// The height is whatever the content needs, so [`size`](View::size) after
/// measuring gives the dismiss threshold: a touch at or below
/// `panel.size().height` landed on the content showing through.
///
/// ```rust,ignore
/// OverlayPanel::new(vstack![gap; brightness, warmth])
///     .scrim(Scrim::Dim)            // push the screen below into the background
///     .on_scrim_tap(Msg::Dismiss)   // a touch down there closes the panel
/// ```
pub struct OverlayPanel<M> {
    content: Box<dyn View<M>>,
    /// `None` defers to the activity's localized title.
    title: Option<String>,
    /// Vertical inset above and below the content, inside the band.
    padding: i32,
    /// Sent when the preserved screen below the panel is tapped.
    scrim_tap: Option<M>,
    /// How that preserved screen is painted.
    scrim: Scrim,
    /// Set during `measure`; where the content is drawn and hit-tested.
    content_origin: Point,
    measured: Size,
}

impl<M: Clone + 'static> OverlayPanel<M> {
    pub fn new(content: impl View<M> + 'static) -> Self {
        OverlayPanel {
            content: Box::new(content),
            title: None,
            padding: 0,
            scrim_tap: None,
            scrim: Scrim::None,
            content_origin: Point::ORIGIN,
            measured: Size::ZERO,
        }
    }

    /// Dismisses on a tap below the panel, where the screen it dropped over is
    /// still showing — the drop-down equivalent of a modal scrim.
    ///
    /// Declared as an ordinary interaction, so the runtime routes it like any
    /// other control and the screen never compares a touch against the panel's
    /// height.
    pub fn on_scrim_tap(mut self, message: M) -> Self {
        self.scrim_tap = Some(message);
        self
    }

    /// Dims the screen showing below the panel, so the panel reads as the
    /// foreground without its context being repainted or hidden.
    ///
    /// ```rust,ignore
    /// OverlayPanel::new(content).scrim(Scrim::Dim)
    /// ```
    pub fn scrim(mut self, scrim: Scrim) -> Self {
        self.scrim = scrim;
        self
    }

    /// Overrides the header title. Prefer the activity's own, already
    /// translated, title.
    pub fn title(mut self, title: impl Into<String>) -> Self {
        self.title = Some(title.into());
        self
    }
}

/// Geometry, which does not depend on the message type.
impl<M> OverlayPanel<M> {
    /// The strip below the panel, where the screen it dropped over still shows.
    /// `None` once the panel fills the screen.
    ///
    /// Both the dimming and the dismiss target come from here, so the region a
    /// tap closes is by construction the region that looks tappable.
    fn below_panel(&self) -> Option<Rect> {
        let screen = Renderer::screen_size();
        let height = screen.height - self.measured.height;
        (height > 0).then(|| Rect::new(0, self.measured.height, screen.width, height))
    }
}

impl<M: Clone> View<M> for OverlayPanel<M> {
    fn measure(&mut self, _available: Size) {
        let screen = Renderer::screen_size();
        let side = Theme::metric(ThemeMetric::ContentSidePadding);
        let header =
            Theme::metric(ThemeMetric::TopPadding) + Theme::metric(ThemeMetric::HeaderHeight);
        self.padding = Theme::metric(ThemeMetric::VerticalSpacing);

        self.content_origin = Point::new(side, header + self.padding);
        // Height is unbounded: a drop-down grows to fit rather than scrolling.
        self.content
            .measure(Size::new(screen.width - side * 2, screen.height - header));

        let height = self.content_origin.y + self.content.size().height + self.padding;
        self.measured = Size::new(screen.width, height.min(screen.height));
    }

    fn size(&self) -> Size {
        self.measured
    }

    fn render(&self, origin: Point) {
        // The screen below is left as it was, so dim it before anything else:
        // the panel then reads as foreground without repainting its context.
        if self.scrim == Scrim::Dim {
            if let Some(below) = self.below_panel() {
                Renderer::scrim(below);
            }
        }

        // Clears only the band, so the screen underneath survives as a scrim.
        Renderer::fill_rect(
            Rect::new(0, 0, self.measured.width, self.measured.height),
            false,
        );

        match &self.title {
            Some(title) => ScreenChrome::draw_header(title),
            None => ScreenChrome::draw_screen_header(),
        }

        self.content
            .render(origin.offset(self.content_origin.x, self.content_origin.y));

        // A solid rule along the bottom edge, so the panel reads as a
        // drop-down over the content rather than a partial repaint.
        Renderer::fill_rect(
            Rect::new(
                0,
                self.measured.height - EDGE_THICKNESS,
                self.measured.width,
                EDGE_THICKNESS,
            ),
            true,
        );
    }

    fn interactions(&mut self, origin: Point, out: &mut Interactions<M>) {
        // The scrim goes first so a control declared later wins the same point;
        // the runtime scans interactions back to front.
        if let (Some(message), Some(below)) = (self.scrim_tap.clone(), self.below_panel()) {
            out.declare(below, InputMask::TAP, Trigger::Message(message));
        }

        let at = origin.offset(self.content_origin.x, self.content_origin.y);
        self.content.interactions(at, out);
    }
}
