//! Text measurement: `cpui`'s [`TextMetrics`] over the firmware's font engine.

use cpui::host::{FontId, FontRole, FontStyle, TextMetrics};

use crate::firmware::{c, Firmware};
use crate::raw;

/// Families as `resolveFont` in the C++ numbers them.
const FAMILY_UI: u8 = 2;

impl TextMetrics for Firmware {
    fn font(&self, role: FontRole) -> FontId {
        // Roles map onto the faces main.cpp registers. `slim` compiles most of
        // them out, in which case this is 0 and the text is skipped rather than
        // drawn with an unregistered id.
        FontId(match role {
            FontRole::Ui => unsafe { raw::cpp_resolve_font(FAMILY_UI, 12) },
            FontRole::UiSmall => unsafe { raw::cpp_resolve_font(FAMILY_UI, 10) },
            FontRole::Reader => unsafe { raw::cpp_reader_font() },
        })
    }

    fn text_width(&self, font: FontId, text: &str, style: FontStyle) -> i32 {
        if !font.is_available() {
            return 0;
        }
        let text = c(text);
        unsafe { raw::cpp_text_width(font.0, text.as_ptr().cast(), style as u8) }.max(0)
    }

    fn line_height(&self, font: FontId) -> i32 {
        if !font.is_available() {
            return 0;
        }
        unsafe { raw::cpp_line_height(font.0) }.max(0)
    }
}
