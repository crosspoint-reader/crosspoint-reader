//! Themed chrome: `cpui`'s [`Chrome`] over the firmware's `UITheme`.
//!
//! Lists and modals are painted by the theme rather than by the framework, so a
//! Rust screen looks identical to a native one and follows the user's theme
//! without `cpui` knowing what a theme is.

use cpui::geometry::Rect;
use cpui::host::{Chrome, Hint, RowField, ThemeMetric};

use crate::cells::{option_cell, row_cell, Cells};
use crate::firmware::{c, Firmware};
use crate::raw;

impl Chrome for Firmware {
    fn metric(&self, metric: ThemeMetric) -> i32 {
        unsafe { raw::cpp_theme_metric(metric as u8) }
    }

    fn draw_header(&self, title: Option<&str>, subtitle: Option<&str>) {
        // A null title asks the firmware for the screen's own translated one.
        let title = title.map(c);
        let subtitle = subtitle.map(c);
        unsafe {
            raw::cpp_theme_draw_header(Cells::ptr(&title), Cells::ptr(&subtitle));
        }
    }

    fn draw_sub_header(&self, rect: Rect, label: &str, right_label: Option<&str>) {
        let label = c(label);
        let right = right_label.map(c);
        unsafe {
            raw::cpp_theme_draw_sub_header(
                rect.x(),
                rect.y(),
                rect.width(),
                rect.height(),
                label.as_ptr().cast(),
                Cells::ptr(&right),
            );
        }
    }

    fn draw_button_hints(&self, back: &Hint, confirm: &Hint, previous: &Hint, next: &Hint) {
        // A null slot means "your standard label"; the firmware also reorders
        // them for the user's front-button layout.
        let slots = [back, confirm, previous, next].map(|hint| hint.label().map(c));
        unsafe {
            raw::cpp_theme_draw_button_hints(
                Cells::ptr(&slots[0]),
                Cells::ptr(&slots[1]),
                Cells::ptr(&slots[2]),
                Cells::ptr(&slots[3]),
            );
        }
    }

    fn draw_progress_bar(&self, rect: Rect, current: u32, total: u32) {
        unsafe {
            raw::cpp_theme_draw_progress_bar(
                rect.x(),
                rect.y(),
                rect.width(),
                rect.height(),
                current,
                total,
            );
        }
    }

    fn draw_slider(&self, rect: Rect, value: i32, max: i32) {
        unsafe {
            raw::cpp_theme_draw_slider(rect.x(), rect.y(), rect.width(), rect.height(), value, max);
        }
    }

    fn draw_list<'a>(
        &self,
        rect: Rect,
        rows: usize,
        selected: i32,
        row: &dyn Fn(usize, RowField) -> Option<&'a str>,
    ) {
        let cells = Cells::rows(rows, row);
        unsafe {
            raw::cpp_theme_draw_list(
                rect.x(),
                rect.y(),
                rect.width(),
                rect.height(),
                rows as i32,
                selected,
                row_cell,
                cells.as_ctx(),
            );
        }
    }

    fn draw_option_popup<'a>(
        &self,
        title: &str,
        options: &dyn Fn(usize) -> Option<&'a str>,
        count: usize,
        selected: i32,
    ) {
        let title = c(title);
        let cells = Cells::options(count, options);
        unsafe {
            raw::cpp_theme_draw_option_popup(
                title.as_ptr().cast(),
                option_cell,
                cells.as_ctx(),
                count as i32,
                selected,
            );
        }
    }

    fn option_popup_row_rect<'a>(
        &self,
        title: &str,
        options: &dyn Fn(usize) -> Option<&'a str>,
        count: usize,
        index: usize,
    ) -> Option<Rect> {
        let title = c(title);
        let cells = Cells::options(count, options);
        let mut xywh = [0i32; 4];
        let found = unsafe {
            raw::cpp_option_popup_row_rect(
                title.as_ptr().cast(),
                option_cell,
                cells.as_ctx(),
                count as i32,
                index as i32,
                xywh.as_mut_ptr(),
            )
        };
        (found != 0).then(|| Rect::new(xywh[0], xywh[1], xywh[2], xywh[3]))
    }

    fn screen_title(&self) -> &str {
        unsafe { crate::borrow_cstr(raw::cpp_activity_title().cast()) }
    }

    fn finish(&self) {
        unsafe { raw::cpp_activity_finish() }
    }

    fn request_update(&self) {
        unsafe { raw::cpp_activity_request_update() }
    }
}
