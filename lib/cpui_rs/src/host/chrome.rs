//! The themed furniture: metrics the theme owns, and the parts it paints
//! itself.
//!
//! A list, a modal and a progress bar are drawn by the host rather than by the
//! framework, so a Rust screen looks identical to a native one and follows the
//! user's theme without the framework knowing what a theme is.

use alloc::string::String;

use crate::geometry::Rect;

/// A geometry value from the active theme.
///
/// Requested one at a time by tag rather than mirrored as a struct: a host's
/// metrics table has dozens of fields, and a copy of its layout here would
/// silently read the wrong values the moment one is inserted.
#[repr(u8)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum ThemeMetric {
    /// Gap above the header band.
    TopPadding = 0,
    HeaderHeight = 1,
    /// The theme's standard gap between stacked elements.
    VerticalSpacing = 2,
    /// Height reserved at the bottom for button hints.
    ButtonHintsHeight = 3,
    ContentSidePadding = 4,
    /// First y below the header that content may use.
    ContentTop = 5,
    /// First y occupied by the button hints; content must stay above.
    ContentBottom = 6,
    ListRowHeight = 7,
    ListRowHeightWithSubtitle = 8,
    /// Space the theme leaves between one row and the next. A list that
    /// measured without it asks for less height than the host needs, and the
    /// host draws only rows that fully fit — so the last one silently vanishes.
    ListRowGap = 14,
    ProgressBarHeight = 9,
    /// Smallest comfortably tappable dimension.
    MinTouchSize = 10,
    /// A slider knob's width, and the padding its track is inset by at each
    /// end. The framework converts a touch to a value with these, so they have
    /// to be the numbers the host actually draws with — asking keeps the two
    /// from drifting when the theme changes them.
    SliderKnobWidth = 11,
    SliderKnobHeight = 12,
    SliderSideInset = 13,
}

/// Which piece of a list row is being asked for.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum RowField {
    Title,
    Subtitle,
    Value,
}

/// Chrome the host draws on the framework's behalf.
pub trait Chrome {
    fn metric(&self, metric: ThemeMetric) -> i32;

    /// The header band, including whatever the host puts in it (a battery
    /// indicator, say). `None` uses the screen's own title.
    fn draw_header(&self, title: Option<&str>, subtitle: Option<&str>);

    fn draw_sub_header(&self, rect: Rect, label: &str, right_label: Option<&str>);

    /// The four hints, given by meaning. The host reorders them to match the
    /// user's button layout; `None` means "your standard label for this slot".
    fn draw_button_hints(&self, back: &Hint, confirm: &Hint, previous: &Hint, next: &Hint);

    fn draw_progress_bar(&self, rect: Rect, current: u32, total: u32);

    /// The themed slider: a track, a fill up to `value`, and a knob over both.
    /// The host owns every dimension of it; the framework says only where it
    /// goes and how far along it is.
    fn draw_slider(&self, rect: Rect, value: i32, max: i32);

    /// The scroll indicator beside a scrolling region: how much of `content`
    /// the `rect`-sized window shows, and how far down it sits. The host draws
    /// nothing when everything already fits.
    fn draw_scroll_indicator(&self, rect: Rect, content: i32, visible: i32, offset: i32);

    /// The themed list. `row` is called back per visible row and field;
    /// returning `None` omits that field, which is how the host decides between
    /// a one- and two-line row.
    fn draw_list<'a>(
        &self,
        rect: Rect,
        rows: usize,
        selected: i32,
        row: &dyn Fn(usize, RowField) -> Option<&'a str>,
    );

    /// The themed modal. `option` is called back per row.
    fn draw_option_popup<'a>(
        &self,
        title: &str,
        options: &dyn Fn(usize) -> Option<&'a str>,
        count: usize,
        selected: i32,
    );

    /// Screen rect of one modal row, so hit-testing does not re-derive the
    /// dialog geometry the host already owns.
    fn option_popup_row_rect<'a>(
        &self,
        title: &str,
        options: &dyn Fn(usize) -> Option<&'a str>,
        count: usize,
        index: usize,
    ) -> Option<Rect>;

    /// The current screen's own title, already translated.
    fn screen_title(&self) -> &str;

    /// Pops this screen.
    fn finish(&self);

    /// Asks for a repaint. E-ink does not refresh on its own.
    fn request_update(&self);
}

/// One button-hint slot.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub enum Hint {
    /// The host's own translated label for this slot.
    #[default]
    Standard,
    /// Leave the slot blank.
    None,
    /// A label this screen supplies.
    Text(String),
}

impl Hint {
    pub fn text(label: impl Into<String>) -> Self {
        Hint::Text(label.into())
    }

    /// What the host should draw: `None` means "your standard label".
    pub fn label(&self) -> Option<&str> {
        match self {
            Hint::Standard => None,
            Hint::None => Some(""),
            Hint::Text(text) => Some(text),
        }
    }
}

/// The active theme, as widgets reach for it.
pub struct Theme;

impl Theme {
    pub fn metric(metric: ThemeMetric) -> i32 {
        super::current().metric(metric)
    }

    /// The region between the header and the button hints, already inset
    /// horizontally by the theme's side padding.
    pub fn content_area() -> Rect {
        let top = Self::metric(ThemeMetric::ContentTop);
        let bottom = Self::metric(ThemeMetric::ContentBottom);
        let side = Self::metric(ThemeMetric::ContentSidePadding);
        let width = super::current().screen_size().width;

        Rect::new(side, top, width - side * 2, bottom - top)
    }

    pub fn draw_sub_header(rect: Rect, label: &str, right_label: Option<&str>) {
        super::current().draw_sub_header(rect, label, right_label)
    }

    pub fn draw_progress_bar(rect: Rect, current: u32, total: u32) {
        super::current().draw_progress_bar(rect, current, total)
    }

    pub fn draw_slider(rect: Rect, value: i32, max: i32) {
        super::current().draw_slider(rect, value, max)
    }

    pub fn draw_scroll_indicator(rect: Rect, content: i32, visible: i32, offset: i32) {
        super::current().draw_scroll_indicator(rect, content, visible, offset)
    }

    /// The themed list. `row` is asked for each field of each visible row;
    /// `None` omits that field, which is how the host chooses a one- or
    /// two-line row.
    pub fn draw_list<'a>(
        rect: Rect,
        rows: usize,
        selected: i32,
        row: &dyn Fn(usize, RowField) -> Option<&'a str>,
    ) {
        super::current().draw_list(rect, rows, selected, row)
    }

    pub fn draw_option_popup<'a>(
        title: &str,
        options: &dyn Fn(usize) -> Option<&'a str>,
        count: usize,
        selected: i32,
    ) {
        super::current().draw_option_popup(title, options, count, selected)
    }

    pub fn option_popup_row_rect<'a>(
        title: &str,
        options: &dyn Fn(usize) -> Option<&'a str>,
        count: usize,
        index: usize,
    ) -> Option<Rect> {
        super::current().option_popup_row_rect(title, options, count, index)
    }
}

/// The screen's own furniture: header band and button hints.
pub struct ScreenChrome;

impl ScreenChrome {
    /// Draws the header with an explicit title.
    pub fn draw_header(title: &str) {
        super::current().draw_header(Some(title), None)
    }

    /// Draws the header with the screen's own translated title.
    ///
    /// The title is fetched here rather than left for the host to substitute:
    /// passing `None` through means "no title" to a `Chrome` implementation,
    /// and a host that took it literally drew an empty header band.
    pub fn draw_screen_header() {
        super::current().draw_header(Some(Self::screen_title()), None)
    }

    pub fn draw_button_hints(back: &Hint, confirm: &Hint, previous: &Hint, next: &Hint) {
        super::current().draw_button_hints(back, confirm, previous, next)
    }

    pub fn screen_title() -> &'static str {
        super::current().screen_title()
    }
}

/// Pops the current screen.
pub fn finish_screen() {
    super::current().finish()
}

/// Asks for a repaint. E-ink does not refresh on its own.
pub fn request_update() {
    super::current().request_update()
}
