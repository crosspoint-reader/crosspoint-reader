//! A deterministic stand-in for a host, so layouts can be tested on a desktop
//! with no firmware and no simulator.
//!
//! Enabled by `cfg(test)` here, and by the `testing` feature for crates that
//! want to test their own screens.
//!
//! This used to be 457 lines of `#[no_mangle] extern "C"` doubles that existed
//! only to make the test binary link. With the [`host`](crate::host) contract
//! it is an ordinary struct: install it and the framework talks to it instead
//! of to a device.
//!
//! ```rust,ignore
//! testing::install();
//! testing::reset();
//! view.measure(testing::screen());
//! view.render(Point::ORIGIN);
//! assert_eq!(testing::drawn_text().len(), 3);
//! ```

use alloc::string::{String, ToString};
use alloc::vec::Vec;
use core::cell::RefCell;

use crate::geometry::{Point, Rect, Size};
use crate::host::{
    Canvas, Chrome, Clock, FontId, FontRole, FontStyle, Hint, IconRef, InputSource, RowField,
    TextMetrics, ThemeMetric,
};
use crate::{Button, SwipeDir};

/// One recorded `draw_text`: position, text, font id and style.
pub type TextDraw = (i32, i32, String, i32, u8);

/// One recorded rectangle: x, y, width, height, and how it was painted.
pub type RectDraw = (i32, i32, i32, i32, RectKind);

/// How a recorded rectangle was painted.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum RectKind {
    Filled,
    Stroked,
    /// Dithered, which reads as grey on a 1-bit panel.
    Dither,
    /// Darkened without erasing, so what was behind stays legible.
    Scrim,
}

/// Screen size the fake reports: an X4 Pro held in portrait.
pub const SCREEN_WIDTH: i32 = 480;
pub const SCREEN_HEIGHT: i32 = 800;

/// Chrome geometry, in the proportions the real themes use.
pub const TOP_PADDING: i32 = 8;
pub const HEADER_HEIGHT: i32 = 40;
pub const VERTICAL_SPACING: i32 = 12;
pub const BUTTON_HINTS_HEIGHT: i32 = 40;
pub const SIDE_PADDING: i32 = 16;
pub const MIN_TOUCH_SIZE: i32 = 44;
pub const LIST_ROW_HEIGHT: i32 = 40;
pub const LIST_ROW_HEIGHT_WITH_SUBTITLE: i32 = 56;
pub const PROGRESS_BAR_HEIGHT: i32 = 6;
pub const CONTENT_TOP: i32 = TOP_PADDING + HEADER_HEIGHT + VERTICAL_SPACING;
pub const CONTENT_BOTTOM: i32 = SCREEN_HEIGHT - BUTTON_HINTS_HEIGHT;

/// Font ids the fake hands out, and their metrics, indexed by role.
const UI_FONT: i32 = 1003;
const UI_SMALL_FONT: i32 = 1004;
const READER_FONT: i32 = 1001;

fn metrics_of(font: i32) -> (i32, i32) {
    // (line height, per-character advance)
    match font {
        UI_SMALL_FONT => (14, 5),
        READER_FONT => (19, 8),
        _ => (17, 6),
    }
}

/// Line height the fake reports for a font id.
pub fn line_height(font_id: i32) -> i32 {
    metrics_of(font_id).0
}

/// Width the fake reports for `text` in a font id.
pub fn text_width(text: &str, font_id: i32) -> i32 {
    text.chars().count() as i32 * metrics_of(font_id).1
}

/// The screen size, for `measure`.
pub fn screen() -> Size {
    Size::new(SCREEN_WIDTH, SCREEN_HEIGHT)
}

thread_local! {
    static DRAWN_TEXT: RefCell<Vec<TextDraw>> = const { RefCell::new(Vec::new()) };
    static DRAWN_RECTS: RefCell<Vec<RectDraw>> = const { RefCell::new(Vec::new()) };
    static DRAWN_HEADERS: RefCell<Vec<Option<String>>> = const { RefCell::new(Vec::new()) };
    static NOW: core::cell::Cell<u32> = const { core::cell::Cell::new(0) };
}

/// Forgets every recorded draw. Call at the start of each test.
pub fn reset() {
    DRAWN_TEXT.with(|drawn| drawn.borrow_mut().clear());
    DRAWN_RECTS.with(|drawn| drawn.borrow_mut().clear());
    DRAWN_HEADERS.with(|drawn| drawn.borrow_mut().clear());
}

/// Every `draw_text` recorded since the last [`reset`].
pub fn drawn_text() -> Vec<TextDraw> {
    DRAWN_TEXT.with(|drawn| drawn.borrow().clone())
}

/// Every rectangle recorded since the last [`reset`], in draw order.
pub fn drawn_rects() -> Vec<RectDraw> {
    DRAWN_RECTS.with(|drawn| drawn.borrow().clone())
}

/// The title passed to each `draw_header` since the last [`reset`]. `None` is a
/// header asked to draw no title at all, which on the real host paints an empty
/// band - the failure this records exists to catch.
pub fn drawn_headers() -> Vec<Option<String>> {
    DRAWN_HEADERS.with(|drawn| drawn.borrow().clone())
}

/// Moves the fake clock, so repeat timing is deterministic.
pub fn set_millis(value: u32) {
    NOW.with(|now| now.set(value));
}

/// The fake host.
pub struct TestHost;

static TEST_HOST: TestHost = TestHost;

/// Installs the fake. Idempotent, so every test may call it.
pub fn install() {
    if !crate::host::is_installed() {
        // Safety: tests are single-threaded per case and nothing has rendered.
        unsafe { crate::host::install(&TEST_HOST) };
    }
}

impl Canvas for TestHost {
    fn screen_size(&self) -> Size {
        screen()
    }

    fn clear(&self) {}

    fn draw_text(&self, origin: Point, text: &str, font: FontId, style: FontStyle) {
        if font.0 == 0 {
            return; // a font this build omitted draws nothing
        }
        DRAWN_TEXT.with(|drawn| {
            drawn
                .borrow_mut()
                .push((origin.x, origin.y, text.to_string(), font.0, style as u8))
        });
    }

    fn fill_rect(&self, rect: Rect, _black: bool) {
        record(rect, RectKind::Filled);
    }

    fn stroke_rect(&self, rect: Rect) {
        record(rect, RectKind::Stroked);
    }

    fn draw_line(&self, _from: Point, _to: Point) {}

    fn fill_rect_dither(&self, rect: Rect, _light: bool) {
        record(rect, RectKind::Dither);
    }

    fn scrim(&self, rect: Rect) {
        record(rect, RectKind::Scrim);
    }

    fn draw_image(&self, _origin: Point, _data: &[u8], _size: Size) {}

    fn draw_icon(&self, _origin: Point, _icon: IconRef) {}

    /// Reports the requested size, so icon layout is predictable.
    fn icon_size(&self, icon: IconRef) -> i32 {
        icon.size
    }
}

fn record(rect: Rect, kind: RectKind) {
    DRAWN_RECTS.with(|drawn| {
        drawn
            .borrow_mut()
            .push((rect.x(), rect.y(), rect.width(), rect.height(), kind))
    });
}

impl TextMetrics for TestHost {
    fn font(&self, role: FontRole) -> FontId {
        FontId(match role {
            FontRole::Ui => UI_FONT,
            FontRole::UiSmall => UI_SMALL_FONT,
            FontRole::Reader => READER_FONT,
        })
    }

    fn text_width(&self, font: FontId, text: &str, _style: FontStyle) -> i32 {
        if font.0 == 0 {
            return 0;
        }
        text_width(text, font.0)
    }

    fn line_height(&self, font: FontId) -> i32 {
        if font.0 == 0 {
            return 0;
        }
        line_height(font.0)
    }
}

impl Chrome for TestHost {
    fn metric(&self, metric: ThemeMetric) -> i32 {
        match metric {
            ThemeMetric::TopPadding => TOP_PADDING,
            ThemeMetric::HeaderHeight => HEADER_HEIGHT,
            ThemeMetric::VerticalSpacing => VERTICAL_SPACING,
            ThemeMetric::ButtonHintsHeight => BUTTON_HINTS_HEIGHT,
            ThemeMetric::ContentSidePadding => SIDE_PADDING,
            ThemeMetric::ContentTop => CONTENT_TOP,
            ThemeMetric::ContentBottom => CONTENT_BOTTOM,
            ThemeMetric::ListRowHeight => LIST_ROW_HEIGHT,
            ThemeMetric::ListRowHeightWithSubtitle => LIST_ROW_HEIGHT_WITH_SUBTITLE,
            ThemeMetric::ProgressBarHeight => PROGRESS_BAR_HEIGHT,
            ThemeMetric::MinTouchSize => MIN_TOUCH_SIZE,
        }
    }

    fn draw_header(&self, title: Option<&str>, _subtitle: Option<&str>) {
        DRAWN_HEADERS.with(|drawn| drawn.borrow_mut().push(title.map(String::from)));
    }

    fn draw_sub_header(&self, _rect: Rect, _label: &str, _right: Option<&str>) {}

    fn draw_button_hints(&self, _back: &Hint, _confirm: &Hint, _prev: &Hint, _next: &Hint) {}

    fn draw_progress_bar(&self, _rect: Rect, _current: u32, _total: u32) {}

    fn draw_list<'a>(
        &self,
        _rect: Rect,
        _rows: usize,
        _selected: i32,
        _row: &dyn Fn(usize, RowField) -> Option<&'a str>,
    ) {
    }

    fn draw_option_popup<'a>(
        &self,
        _title: &str,
        _options: &dyn Fn(usize) -> Option<&'a str>,
        _count: usize,
        _selected: i32,
    ) {
    }

    /// Rows stacked from the middle of the screen, so hit-testing a modal is
    /// predictable without reproducing the real dialog's geometry.
    fn option_popup_row_rect<'a>(
        &self,
        _title: &str,
        _options: &dyn Fn(usize) -> Option<&'a str>,
        count: usize,
        index: usize,
    ) -> Option<Rect> {
        if index >= count {
            return None;
        }
        let top = SCREEN_HEIGHT / 4;
        Some(Rect::new(
            SIDE_PADDING,
            top + index as i32 * LIST_ROW_HEIGHT,
            SCREEN_WIDTH - SIDE_PADDING * 2,
            LIST_ROW_HEIGHT,
        ))
    }

    fn screen_title(&self) -> &str {
        "Test"
    }

    fn finish(&self) {}

    fn request_update(&self) {}
}

/// No input by default. A test that needs a touch drives the view directly.
impl InputSource for TestHost {
    fn was_pressed(&self, _button: Button) -> bool {
        false
    }

    fn is_pressed(&self, _button: Button) -> bool {
        false
    }

    fn was_released(&self, _button: Button) -> bool {
        false
    }

    fn has_touch(&self) -> bool {
        false
    }

    fn tap(&self) -> Option<Point> {
        None
    }

    fn touch_held(&self) -> Option<Point> {
        None
    }

    fn touch_released(&self) -> bool {
        false
    }

    fn swipe(&self) -> SwipeDir {
        SwipeDir::None
    }

    fn was_back_gesture(&self) -> bool {
        false
    }

    fn was_home_gesture(&self) -> bool {
        false
    }
}

impl Clock for TestHost {
    fn millis(&self) -> u32 {
        NOW.with(|now| now.get())
    }
}
