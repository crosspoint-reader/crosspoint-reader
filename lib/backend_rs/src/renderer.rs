//! The framebuffer: `xpui`'s [`Canvas`] over the firmware's `GfxRenderer`.

use xpui::geometry::{Point, Rect, Size};
use xpui::host::{Canvas, FontId, FontStyle, IconRef};

use crate::firmware::{Firmware, c};
use crate::raw;

impl Canvas for Firmware {
    fn screen_size(&self) -> Size {
        Size::new(raw::cpp_screen_width(), raw::cpp_screen_height())
    }

    fn clear(&self) {
        raw::cpp_clear_screen()
    }

    fn draw_text(&self, origin: Point, text: &str, font: FontId, style: FontStyle) {
        if !font.is_available() {
            return; // a face this build omitted draws nothing
        }
        let text = c(text);
        unsafe {
            raw::cpp_draw_text(
                origin.x,
                origin.y,
                text.as_ptr().cast(),
                font.0,
                style as u8,
            )
        }
    }

    fn fill_rect(&self, rect: Rect, black: bool) {
        raw::cpp_draw_rect(
            rect.x(),
            rect.y(),
            rect.width(),
            rect.height(),
            1,
            u8::from(black),
        )
    }

    fn stroke_rect(&self, rect: Rect) {
        raw::cpp_draw_rect(rect.x(), rect.y(), rect.width(), rect.height(), 0, 1)
    }

    fn draw_line(&self, from: Point, to: Point) {
        raw::cpp_draw_line(from.x, from.y, to.x, to.y)
    }

    fn fill_rect_dither(&self, rect: Rect, light: bool) {
        raw::cpp_fill_rect_dither(
            rect.x(),
            rect.y(),
            rect.width(),
            rect.height(),
            u8::from(light),
        )
    }

    fn set_clip(&self, rect: Option<Rect>) {
        // A zero-sized rect is the "no clip" signal, so the boundary stays one
        // function instead of two.
        let rect = rect.unwrap_or(Rect::new(0, 0, 0, 0));
        raw::cpp_set_clip(rect.x(), rect.y(), rect.width(), rect.height())
    }

    fn scrim(&self, rect: Rect) {
        raw::cpp_scrim(rect.x(), rect.y(), rect.width(), rect.height())
    }

    fn draw_image(&self, origin: Point, data: &[u8], size: Size) {
        // Refuse a buffer too short for its stated size rather than letting the
        // C side read past the end.
        let needed = (size.width as usize).div_ceil(8) * size.height as usize;
        if data.len() < needed {
            return;
        }
        unsafe {
            raw::cpp_draw_image(data.as_ptr(), origin.x, origin.y, size.width, size.height);
        }
    }

    fn draw_icon(&self, origin: Point, icon: IconRef) {
        raw::cpp_draw_icon(icon.kind as u8, icon.variant, icon.size, origin.x, origin.y)
    }

    fn icon_size(&self, icon: IconRef) -> i32 {
        raw::cpp_icon_size(icon.kind as u8, icon.variant, icon.size)
    }
}
