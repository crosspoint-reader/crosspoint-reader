//! Bitmaps.

use crate::geometry::{Point, Size};
use crate::host::{IconRef, Renderer};
use crate::view::View;

/// A 1-bit bitmap.
///
/// # Buffer format
///
/// Row-major, top row first, MSB first within each byte, `(width + 7) / 8`
/// bytes per row with each row padded independently — and **bit 0 is ink,
/// bit 1 is white**. That polarity is inverted from the usual convention, so a
/// buffer produced elsewhere will very likely render as a negative.
///
/// The data must outlive the widget, which is why this borrows rather than
/// copying: the firmware's own assets are `static const` arrays in flash, and
/// copying a 1.8 KB logo onto a 200 KB heap to draw it would be absurd.
///
/// ```rust,ignore
/// Image::new(LOGO_120, 120, 120)
/// ```
pub struct Image {
    data: &'static [u8],
    source: Size,
    measured: Size,
}

impl Image {
    /// Wraps a bitmap of `width` x `height` pixels.
    ///
    /// Renders nothing if the buffer is too short for those dimensions, rather
    /// than reading past the end of it.
    pub fn new(data: &'static [u8], width: i32, height: i32) -> Self {
        Image {
            data,
            source: Size::new(width, height),
            measured: Size::ZERO,
        }
    }

    /// Whether the buffer is long enough for the stated dimensions.
    fn is_valid(&self) -> bool {
        let row_bytes = (self.source.width as usize).div_ceil(8);
        !self.source.is_empty() && self.data.len() >= row_bytes * self.source.height as usize
    }
}

impl<M> View<M> for Image {
    fn measure(&mut self, _available: Size) {
        // A bitmap has no scaling in the renderer, so it occupies exactly its
        // own size regardless of what it is offered.
        self.measured = if self.is_valid() {
            self.source
        } else {
            Size::ZERO
        };
    }

    fn size(&self) -> Size {
        self.measured
    }

    fn render(&self, origin: Point) {
        if self.measured.is_empty() {
            return;
        }
        Renderer::draw_image(origin, self.data, self.source);
    }
}

/// An icon from the host firmware's registry.
///
/// Kept separate from [`Image`] because the two use different asset
/// conventions and are not interchangeable. Icons are drawn transparently and
/// pixel by pixel, so they follow the screen orientation; a raw bitmap does
/// not. Sizes come from the registry, so an icon always occupies its natural
/// size.
///
/// ```rust,ignore
/// Icon::new(IconRole::Sun).filled(self.light_on)
/// Icon::new(IconRole::Folder).size(24)
/// ```
pub struct Icon {
    spec: IconRef,
    measured: Size,
}

impl Icon {
    /// An icon for a role, at the theme's usual 32px.
    pub fn new(icon: impl Into<IconRef>) -> Self {
        Icon {
            spec: icon.into(),
            measured: Size::ZERO,
        }
    }

    /// Solid rather than outline, where the role has both variants — the cue
    /// the panel uses for "light is on".
    pub fn filled(mut self, filled: bool) -> Self {
        self.spec.variant = u8::from(filled);
        self
    }

    /// Preferred edge length; the host draws the nearest size it ships.
    pub fn size(mut self, size: i32) -> Self {
        self.spec.size = size;
        self
    }
}

impl<M> View<M> for Icon {
    fn measure(&mut self, _available: Size) {
        // An unknown id reports zero and draws nothing, rather than painting
        // garbage at an arbitrary size.
        let size = Renderer::icon_size(self.spec);
        self.measured = Size::new(size, size);
    }

    fn size(&self) -> Size {
        self.measured
    }

    fn render(&self, origin: Point) {
        if self.measured.is_empty() {
            return;
        }
        Renderer::draw_icon(origin, self.spec);
    }
}
