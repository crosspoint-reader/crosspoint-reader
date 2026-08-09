//! Drawing primitives.

use crate::geometry::{Point, Rect, Size};
use crate::host::metrics::{FontId, FontStyle};

/// An icon the host owns.
///
/// Deliberately opaque numbers: which glyph these select is the host's
/// business. Naming them here would tie the framework to one product's assets.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct IconRef {
    /// Which icon, in whatever numbering the host uses.
    pub kind: u16,
    /// A variant of the same icon, where the host offers one (solid vs outline).
    pub variant: u8,
    /// Preferred edge length; the host picks the nearest size it ships.
    pub size: i32,
}

impl IconRef {
    pub fn new(kind: u16) -> Self {
        IconRef {
            kind,
            variant: 0,
            size: 32,
        }
    }
}

/// The framebuffer, as the framework sees it.
pub trait Canvas {
    fn screen_size(&self) -> Size;

    /// Clears to background. A screen painting over what is already there —
    /// an overlay — must not call this.
    fn clear(&self);

    fn draw_text(&self, origin: Point, text: &str, font: FontId, style: FontStyle);

    /// Fills `rect`; `black` false means background.
    fn fill_rect(&self, rect: Rect, black: bool);

    fn stroke_rect(&self, rect: Rect);

    fn draw_line(&self, from: Point, to: Point);

    /// Fills with a 50% dither, which reads as grey on a 1-bit panel.
    fn fill_rect_dither(&self, rect: Rect, light: bool);

    /// Darkens `rect` while leaving what is already drawn there legible.
    ///
    /// Unlike [`fill_rect_dither`](Canvas::fill_rect_dither), which clears
    /// before it patterns, this only adds ink — on one checkerboard parity, so
    /// about half of what was behind survives. Use it to push a background
    /// back behind an overlay without repainting it.
    fn scrim(&self, rect: Rect);

    /// Draws a 1-bpp bitmap. Row-major, MSB first, `(w + 7) / 8` bytes per row,
    /// and **bit 0 is ink** — inverted from the usual convention.
    fn draw_image(&self, origin: Point, data: &[u8], size: Size);

    fn draw_icon(&self, origin: Point, icon: IconRef);

    /// Edge length the host would actually draw, or 0 if it ships nothing for
    /// this icon.
    fn icon_size(&self, icon: IconRef) -> i32;
}

/// The framebuffer, as widgets reach for it.
///
/// A thin façade over the installed [`Canvas`] so a widget writes
/// `Renderer::fill_rect(..)` rather than plumbing a host reference through
/// every call.
pub struct Renderer;

impl Renderer {
    pub fn screen_size() -> Size {
        super::current().screen_size()
    }

    pub fn screen_bounds() -> Rect {
        Rect {
            origin: Point::ORIGIN,
            size: Self::screen_size(),
        }
    }

    pub fn clear() {
        super::current().clear()
    }

    pub fn draw_text(origin: Point, text: &str, font: FontId, style: FontStyle) {
        super::current().draw_text(origin, text, font, style)
    }

    pub fn fill_rect(rect: Rect, black: bool) {
        super::current().fill_rect(rect, black)
    }

    pub fn stroke_rect(rect: Rect) {
        super::current().stroke_rect(rect)
    }

    pub fn draw_line(from: Point, to: Point) {
        super::current().draw_line(from, to)
    }

    pub fn fill_rect_dither(rect: Rect, light: bool) {
        super::current().fill_rect_dither(rect, light)
    }

    pub fn scrim(rect: Rect) {
        super::current().scrim(rect)
    }

    pub fn draw_image(origin: Point, data: &[u8], size: Size) {
        super::current().draw_image(origin, data, size)
    }

    pub fn draw_icon(origin: Point, icon: IconRef) {
        super::current().draw_icon(origin, icon)
    }

    pub fn icon_size(icon: IconRef) -> i32 {
        super::current().icon_size(icon)
    }
}
